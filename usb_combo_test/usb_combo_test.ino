/*
 * Prueba final conjunta: hub Atolla con escaner CDC-ACM + impresora Printer Class.
 *
 * Un unico stack host (USB Host del ESP-IDF) maneja ambos dispositivos.
 * Cuando el escaner lee un payload "LOCKER:<n>" (1..5), la impresora emite
 * un ticket en INGLES con el numero de locker.
 *
 * Compilar con USB Mode = "Hardware CDC and JTAG" (USBMode=hwcdc).
 * Monitor por el puerto UART (CH343).
 */

#include <Arduino.h>
#include "soc/soc_caps.h"

#if !SOC_USB_OTG_SUPPORTED
#error "Este sketch necesita un target con USB OTG (ESP32-S3/P4)."
#endif

#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

extern "C" void USBHostBoardInit(void);

#define MAX_OPEN_DEVICES 8
#define TRANSFER_TIMEOUT 5000
#define DEDUP_WINDOW_MS  5000

struct PrintJob {
  usb_device_handle_t dev;
  uint8_t  iface;
  uint8_t  alt;
  uint8_t  out_ep;
  uint16_t mps;
  bool     valid;
};

struct CdcJob {
  usb_device_handle_t dev;
  uint8_t  comm_iface;
  uint8_t  data_iface;
  uint8_t  alt;
  uint8_t  in_ep;
  uint16_t mps;
  bool     valid;
};

static usb_host_client_handle_t s_client = NULL;
static usb_device_handle_t s_devices[MAX_OPEN_DEVICES];
static QueueHandle_t s_cdcQueue = NULL;
static SemaphoreHandle_t s_printerSem = NULL;
static SemaphoreHandle_t s_scannerSem = NULL;
static SemaphoreHandle_t s_printMutex = NULL;
static PrintJob s_printerJob = {};

static char s_line[64];
static uint8_t s_lineLen = 0;
static char s_lastPayload[64] = "";
static uint32_t s_lastMillis = 0;

#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
static bool enumFilter(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
  (void)dev_desc;
  *bConfigurationValue = 1;
  return true;
}
#endif

static void printerCb(usb_transfer_t *t) { (void)t; xSemaphoreGive(s_printerSem); }
static void scannerCb(usb_transfer_t *t) { (void)t; xSemaphoreGive(s_scannerSem); }

static void printText(const PrintJob &job, const char *text) {
  xSemaphoreTake(s_printMutex, portMAX_DELAY);

  if (!job.valid) {
    Serial0.println("[print] impresora no disponible");
    xSemaphoreGive(s_printMutex);
    return;
  }

  esp_err_t err = usb_host_interface_claim(s_client, job.dev, job.iface, job.alt);
  if (err != ESP_OK) {
    Serial0.printf("[print] claim fallo: %s\n", esp_err_to_name(err));
    xSemaphoreGive(s_printMutex);
    return;
  }

  uint8_t buf[256];
  size_t n = 0;
  buf[n++] = 0x1B; buf[n++] = 0x40;                    // ESC @ inicializar
  buf[n++] = 0x1B; buf[n++] = 0x61; buf[n++] = 0x01;   // ESC a 1 centrar

  size_t tl = strlen(text);
  if (n + tl + 9 > sizeof(buf)) {
    tl = sizeof(buf) - n - 9;
  }
  memcpy(buf + n, text, tl);
  n += tl;

  buf[n++] = 0x1B; buf[n++] = 0x61; buf[n++] = 0x00;   // ESC a 0 izquierda
  buf[n++] = 0x1B; buf[n++] = 0x64; buf[n++] = 0x03;   // ESC d 3 avanzar
  buf[n++] = 0x1D; buf[n++] = 0x56; buf[n++] = 0x00;   // GS V 0 cortar

  usb_transfer_t *xfer = NULL;
  if (usb_host_transfer_alloc(n, 0, &xfer) != ESP_OK || xfer == NULL) {
    Serial0.println("[print] alloc fallo");
    usb_host_interface_release(s_client, job.dev, job.iface);
    xSemaphoreGive(s_printMutex);
    return;
  }

  memcpy(xfer->data_buffer, buf, n);
  xfer->device_handle    = job.dev;
  xfer->bEndpointAddress = job.out_ep;
  xfer->num_bytes        = n;
  xfer->timeout_ms       = TRANSFER_TIMEOUT;
  xfer->callback         = printerCb;
  xfer->context          = NULL;

  Serial0.printf("[print] %u bytes -> ep 0x%02X\n", (unsigned)n, job.out_ep);
  err = usb_host_transfer_submit(xfer);
  if (err != ESP_OK) {
    Serial0.printf("[print] submit fallo: %s\n", esp_err_to_name(err));
  } else if (xSemaphoreTake(s_printerSem, pdMS_TO_TICKS(TRANSFER_TIMEOUT)) == pdTRUE) {
    Serial0.printf("[print] status=%d actual=%d/%u\n", (int)xfer->status,
                   xfer->actual_num_bytes, (unsigned)n);
  } else {
    Serial0.println("[print] timeout");
    usb_host_endpoint_flush(job.dev, job.out_ep);
    xSemaphoreTake(s_printerSem, pdMS_TO_TICKS(1000));
  }

  usb_host_transfer_free(xfer);
  usb_host_interface_release(s_client, job.dev, job.iface);
  xSemaphoreGive(s_printMutex);
}

/*
 * Acepta dos formatos de QR:
 *   LOCKER:<n>            (el que manda la HMI Nextion)
 *   L=<n>&I=..&T=..       (el ticket que ya esta impreso)
 */
static bool parseLocker(const char *payload, int *num) {
  const char *p = NULL;
  if (strncmp(payload, "LOCKER:", 7) == 0) {
    p = payload + 7;
  } else if (strncmp(payload, "L=", 2) == 0) {
    p = payload + 2;
  } else {
    return false;
  }

  if (*p < '0' || *p > '9') {
    return false;
  }

  int n = 0;
  while (*p >= '0' && *p <= '9') {
    n = n * 10 + (*p - '0');
    p++;
  }
  if (*p != '\0' && *p != '&') {
    return false;
  }
  if (n < 1 || n > 5) {
    return false;
  }

  *num = n;
  return true;
}

static void processPayload(const char *payload) {
  uint32_t now = millis();
  if (strcmp(payload, s_lastPayload) == 0 && (now - s_lastMillis) < DEDUP_WINDOW_MS) {
    return;
  }
  strncpy(s_lastPayload, payload, sizeof(s_lastPayload) - 1);
  s_lastPayload[sizeof(s_lastPayload) - 1] = '\0';
  s_lastMillis = now;

  Serial0.printf("[SCAN] %s\n", payload);

  int locker = 0;
  if (parseLocker(payload, &locker)) {
    char msg[96];
    snprintf(msg, sizeof(msg),
             "LOCKER %d\nACCESS GRANTED\n\nTHANK YOU\n", locker);
    Serial0.printf("[print] ticket en ingles para locker %d\n", locker);
    printText(s_printerJob, msg);
  } else {
    Serial0.println("[scan] payload no reconocido (se espera LOCKER:<1..5>)");
  }
}

static void feedByte(uint8_t c) {
  if (c == 0x09 || c == '\r' || c == '\n') {
    if (s_lineLen > 0) {
      s_line[s_lineLen] = '\0';
      processPayload(s_line);
      s_lineLen = 0;
    }
    return;
  }
  if (c >= 0x20 && c < 0x7F) {
    if (s_lineLen < sizeof(s_line) - 1) {
      s_line[s_lineLen++] = (char)c;
    } else {
      s_lineLen = 0;
    }
  }
}

static esp_err_t cdcControl(usb_device_handle_t dev, uint8_t reqType, uint8_t req,
                            uint16_t value, uint16_t index, const uint8_t *data, uint16_t len) {
  size_t bufSize = 8 + len;
  usb_transfer_t *xfer = NULL;
  esp_err_t err = usb_host_transfer_alloc(bufSize, 0, &xfer);
  if (err != ESP_OK || xfer == NULL) {
    return err;
  }

  xfer->data_buffer[0] = reqType;
  xfer->data_buffer[1] = req;
  xfer->data_buffer[2] = value & 0xFF;
  xfer->data_buffer[3] = value >> 8;
  xfer->data_buffer[4] = index & 0xFF;
  xfer->data_buffer[5] = index >> 8;
  xfer->data_buffer[6] = len & 0xFF;
  xfer->data_buffer[7] = len >> 8;
  if (len && data && (reqType & 0x80) == 0) {
    memcpy(xfer->data_buffer + 8, data, len);
  }

  xfer->device_handle    = dev;
  xfer->bEndpointAddress = 0;
  xfer->num_bytes        = bufSize;
  xfer->timeout_ms       = 1000;
  xfer->callback         = scannerCb;
  xfer->context          = NULL;

  err = usb_host_transfer_submit_control(s_client, xfer);
  if (err == ESP_OK) {
    if (xSemaphoreTake(s_scannerSem, pdMS_TO_TICKS(2000)) == pdTRUE) {
      err = (xfer->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
    } else {
      err = ESP_ERR_TIMEOUT;
    }
  }

  usb_host_transfer_free(xfer);
  return err;
}

static void scannerRun(const CdcJob &job) {
  esp_err_t err = usb_host_interface_claim(s_client, job.dev, job.data_iface, job.alt);
  if (err != ESP_OK) {
    Serial0.printf("[scan] claim data iface fallo: %s\n", esp_err_to_name(err));
    return;
  }

  const uint8_t lineCoding[7] = {0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x08}; /* 9600 8N1 */
  err = cdcControl(job.dev, 0x21, 0x20, 0, job.comm_iface, lineCoding, 7);
  Serial0.printf("[scan] SET_LINE_CODING: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));

  err = cdcControl(job.dev, 0x21, 0x22, 0x0003, job.comm_iface, NULL, 0);
  Serial0.printf("[scan] SET_CONTROL_LINE_STATE(DTR|RTS): %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));

  usb_transfer_t *xfer = NULL;
  if (usb_host_transfer_alloc(job.mps, 0, &xfer) != ESP_OK || xfer == NULL) {
    Serial0.println("[scan] alloc fallo");
    usb_host_interface_release(s_client, job.dev, job.data_iface);
    return;
  }

  xfer->device_handle    = job.dev;
  xfer->bEndpointAddress = job.in_ep;
  xfer->callback         = scannerCb;
  xfer->context          = NULL;

  Serial0.printf("[scan] leyendo ep 0x%02X mps=%u — escanea LOCKER:<n>\n", job.in_ep, job.mps);

  while (true) {
    xfer->num_bytes = job.mps;
    err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK) {
      Serial0.printf("[scan] submit fallo: %s\n", esp_err_to_name(err));
      break;
    }
    if (xSemaphoreTake(s_scannerSem, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
      for (int i = 0; i < xfer->actual_num_bytes; i++) {
        feedByte(xfer->data_buffer[i]);
      }
    } else if (xfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
               xfer->status == USB_TRANSFER_STATUS_CANCELED) {
      Serial0.println("[scan] dispositivo retirado");
      break;
    } else {
      Serial0.printf("[scan] status=%d, reintentando\n", (int)xfer->status);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  usb_host_transfer_free(xfer);
  usb_host_interface_release(s_client, job.dev, job.data_iface);
}

static void scannerTask(void *) {
  CdcJob job;
  while (true) {
    if (xQueueReceive(s_cdcQueue, &job, portMAX_DELAY) == pdTRUE) {
      scannerRun(job);
    }
  }
}

static void deviceAdded(uint8_t address) {
  usb_device_handle_t dev = NULL;
  if (usb_host_device_open(s_client, address, &dev) != ESP_OK) {
    Serial0.printf("[usb] no se pudo abrir %u\n", address);
    return;
  }

  const usb_device_desc_t *desc = NULL;
  usb_host_get_device_descriptor(dev, &desc);
  const usb_config_desc_t *cfg = NULL;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || cfg == NULL) {
    Serial0.println("[usb] sin configuracion activa");
    usb_host_device_close(s_client, dev);
    return;
  }

  PrintJob pj = {};
  CdcJob   cj = {};
  pj.dev = dev;
  cj.dev = dev;
  cj.comm_iface = 0xFF;

  int offset = 0;
  const usb_intf_desc_t *curItf = NULL;
  const usb_standard_desc_t *cur = (const usb_standard_desc_t *)cfg;

  while ((cur = usb_parse_next_descriptor(cur, cfg->wTotalLength, &offset)) != NULL) {
    if (cur->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      curItf = (const usb_intf_desc_t *)cur;
      if (curItf->bInterfaceClass == 0x02) {
        cj.comm_iface = curItf->bInterfaceNumber;
      }
      if (curItf->bInterfaceClass == 0x0A) {
        cj.data_iface = curItf->bInterfaceNumber;
        cj.alt = curItf->bAlternateSetting;
      }
    } else if (cur->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && curItf != NULL) {
      const usb_ep_desc_t *ep = (const usb_ep_desc_t *)cur;
      uint8_t type = ep->bmAttributes & 0x03;
      if (type != 0x02) {
        continue;
      }
      if (curItf->bInterfaceClass == 0x07 && !(ep->bEndpointAddress & 0x80) && pj.out_ep == 0) {
        pj.iface  = curItf->bInterfaceNumber;
        pj.alt    = curItf->bAlternateSetting;
        pj.out_ep = ep->bEndpointAddress;
        pj.mps    = ep->wMaxPacketSize & 0x7FF;
      }
      if (curItf->bInterfaceClass == 0x0A && (ep->bEndpointAddress & 0x80) && cj.in_ep == 0) {
        cj.in_ep = ep->bEndpointAddress;
        cj.mps   = ep->wMaxPacketSize & 0x7FF;
      }
    }
  }

  if (desc) {
    Serial0.printf("[usb] %04X:%04X class=0x%02X printer=%d cdc=%d\n",
                   desc->idVendor, desc->idProduct, desc->bDeviceClass,
                   pj.out_ep != 0, cj.in_ep != 0);
  }

  if (pj.out_ep != 0) {
    pj.valid = true;
    s_printerJob = pj;
    Serial0.printf("[usb] impresora lista: iface %u ep 0x%02X\n", pj.iface, pj.out_ep);
  }
  if (cj.in_ep != 0 && cj.comm_iface != 0xFF) {
    cj.valid = true;
    Serial0.printf("[usb] escaner: comm %u data %u ep 0x%02X\n", cj.comm_iface, cj.data_iface, cj.in_ep);
    xQueueSend(s_cdcQueue, &cj, 0);
  }

  for (int i = 0; i < MAX_OPEN_DEVICES; i++) {
    if (s_devices[i] == NULL) {
      s_devices[i] = dev;
      return;
    }
  }
  usb_host_device_close(s_client, dev);
}

static void deviceGone(usb_device_handle_t dev) {
  for (int i = 0; i < MAX_OPEN_DEVICES; i++) {
    if (s_devices[i] == dev) {
      s_devices[i] = NULL;
      break;
    }
  }
  if (s_printerJob.dev == dev) {
    s_printerJob.valid = false;
  }
  usb_host_device_close(s_client, dev);
  Serial0.println("[usb] retirado");
}

static void clientEvent(const usb_host_client_event_msg_t *msg, void *) {
  switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:  deviceAdded(msg->new_dev.address); break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE: deviceGone(msg->dev_gone.dev_hdl); break;
    default: break;
  }
}

static void daemonTask(void *) {
  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

static void clientTask(void *) {
  while (true) {
    usb_host_client_handle_events(s_client, portMAX_DELAY);
  }
}

void setup() {
  Serial0.begin(115200);
  delay(800);
  Serial0.println();
  Serial0.println("=== TEST FINAL: escaneo LOCKER:<n> -> ticket en ingles ===");

  s_cdcQueue   = xQueueCreate(4, sizeof(CdcJob));
  s_printerSem = xSemaphoreCreateBinary();
  s_scannerSem = xSemaphoreCreateBinary();
  s_printMutex = xSemaphoreCreateMutex();

  USBHostBoardInit();

  usb_host_config_t host_config = {};
  host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
  host_config.enum_filter_cb = enumFilter;
#endif

  esp_err_t err = usb_host_install(&host_config);
  if (err != ESP_OK) {
    Serial0.printf("[usb] install fallo: %s\n", esp_err_to_name(err));
    return;
  }

  usb_host_client_config_t client_config = {};
  client_config.is_synchronous = false;
  client_config.max_num_event_msg = 5;
  client_config.async.client_event_callback = clientEvent;
  client_config.async.callback_arg = NULL;

  err = usb_host_client_register(&client_config, &s_client);
  if (err != ESP_OK) {
    Serial0.printf("[usb] client_register fallo: %s\n", esp_err_to_name(err));
    return;
  }

  xTaskCreate(daemonTask,  "usbh_daemon", 4096, NULL, 2, NULL);
  xTaskCreate(clientTask,  "usbh_client", 4096, NULL, 3, NULL);
  xTaskCreate(scannerTask, "scanner",     4096, NULL, 4, NULL);

  Serial0.println("Host listo. Escanea un QR LOCKER:<1..5>.");
}

void loop() {
  delay(1000);
}
