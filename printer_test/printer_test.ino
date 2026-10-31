/*
 * Prueba 2: impresora Kinwodon POS-8360L por USB (VID 0416 / PID 5011),
 * conectada directa al puerto USB-OTG nativo del ESP32-S3, sin hub.
 *
 * El core no trae clase host "USB Printer", asi que se usa la libreria
 * USB Host del ESP-IDF: se enumera el dispositivo, se busca el endpoint
 * BULK OUT y se le envian los bytes ESC/POS directamente.
 *
 * IMPORTANTE:
 *  - Compilar con USB Mode = "Hardware CDC and JTAG" (USBMode=hwcdc),
 *    NO con TinyUSB, para que el host IDF sea dueno del periferico.
 *  - El monitor va por el puerto UART (CH343), no por el USB nativo.
 *  - No se debe llamar a USBHost.begin() (esa es la pila TinyUSB).
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

#define PRINTER_VID        0x0416
#define PRINTER_PID        0x5011
#define MAX_OPEN_DEVICES   8
#define TRANSFER_TIMEOUT   8000

struct PrintJob {
  usb_device_handle_t dev;
  uint8_t  iface;
  uint8_t  alt;
  uint8_t  out_ep;
  uint16_t mps;
  bool     valid;
};

static usb_host_client_handle_t s_client = NULL;
static usb_device_handle_t s_devices[MAX_OPEN_DEVICES];
static QueueHandle_t s_jobQueue = NULL;
static SemaphoreHandle_t s_xferDone = NULL;
static SemaphoreHandle_t s_printMutex = NULL;
static PrintJob s_lastJob = {};

#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
static bool enumFilter(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
  (void)dev_desc;
  *bConfigurationValue = 1;
  return true;
}
#endif

static void xferCallback(usb_transfer_t *t) {
  (void)t;
  xSemaphoreGive(s_xferDone);
}

/* Ticket de prueba: texto, avance y corte (sin QR, para aislar el transporte). */
static const uint8_t kTicket[] = {
  0x1B, 0x40,                          // ESC @  inicializar
  0x1B, 0x61, 0x01,                    // ESC a 1 centrar
  'P','R','U','E','B','A',' ','I','M','P','R','E','S','O','R','A','\n',
  'E','S','P','3','2','-','S','3',' ','U','S','B','\n',
  0x1B, 0x61, 0x00,                    // ESC a 0 izquierda
  'V','I','D',' ','0','4','1','6',' ','P','I','D',' ','5','0','1','1','\n',
  0x1B, 0x64, 0x04,                    // ESC d 4 avanzar
  0x1D, 0x56, 0x00,                    // GS V 0 cortar
};

static bool findBulkOut(usb_device_handle_t dev, PrintJob &job) {
  const usb_config_desc_t *cfg = NULL;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || cfg == NULL) {
    Serial0.println("[desc] sin configuracion activa");
    return false;
  }

  Serial0.printf("[cfg] interfaces=%u wTotalLength=%u\n", cfg->bNumInterfaces, cfg->wTotalLength);

  int offset = 0;
  const usb_intf_desc_t *curItf = NULL;
  const usb_standard_desc_t *cur = (const usb_standard_desc_t *)cfg;

  while ((cur = usb_parse_next_descriptor(cur, cfg->wTotalLength, &offset)) != NULL) {
    if (cur->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      curItf = (const usb_intf_desc_t *)cur;
      Serial0.printf("[itf] num=%u alt=%u class=0x%02X sub=0x%02X proto=0x%02X eps=%u\n",
                     curItf->bInterfaceNumber, curItf->bAlternateSetting,
                     curItf->bInterfaceClass, curItf->bInterfaceSubClass,
                     curItf->bInterfaceProtocol, curItf->bNumEndpoints);
    } else if (cur->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && curItf != NULL) {
      const usb_ep_desc_t *ep = (const usb_ep_desc_t *)cur;
      uint8_t type = ep->bmAttributes & 0x03;
      const char *typeName = (type == 0x02) ? "bulk" : (type == 0x03) ? "intr" : (type == 0x01) ? "isoc" : "ctrl";
      Serial0.printf("  [ep] addr=0x%02X %s %s mps=%u\n",
                     ep->bEndpointAddress, (ep->bEndpointAddress & 0x80) ? "IN" : "OUT",
                     typeName, (unsigned)(ep->wMaxPacketSize & 0x7FF));

      if (job.out_ep == 0 && type == 0x02 && !(ep->bEndpointAddress & 0x80)) {
        job.iface  = curItf->bInterfaceNumber;
        job.alt    = curItf->bAlternateSetting;
        job.out_ep = ep->bEndpointAddress;
        job.mps    = ep->wMaxPacketSize & 0x7FF;
      }
    }
  }
  return job.out_ep != 0;
}

static void printTest(const PrintJob &job) {
  xSemaphoreTake(s_printMutex, portMAX_DELAY);

  esp_err_t err = usb_host_interface_claim(s_client, job.dev, job.iface, job.alt);
  if (err != ESP_OK) {
    Serial0.printf("[print] interface_claim fallo: %s\n", esp_err_to_name(err));
    xSemaphoreGive(s_printMutex);
    return;
  }

  const size_t len = sizeof(kTicket);
  usb_transfer_t *xfer = NULL;
  err = usb_host_transfer_alloc(len, 0, &xfer);
  if (err != ESP_OK || xfer == NULL) {
    Serial0.printf("[print] transfer_alloc fallo: %s\n", esp_err_to_name(err));
    usb_host_interface_release(s_client, job.dev, job.iface);
    xSemaphoreGive(s_printMutex);
    return;
  }

  memcpy(xfer->data_buffer, kTicket, len);
  xfer->device_handle    = job.dev;
  xfer->bEndpointAddress = job.out_ep;
  xfer->num_bytes        = len;
  xfer->timeout_ms       = TRANSFER_TIMEOUT;
  xfer->callback         = xferCallback;
  xfer->context          = NULL;

  Serial0.printf("[print] enviando %u bytes a ep 0x%02X...\n", (unsigned)len, job.out_ep);
  err = usb_host_transfer_submit(xfer);
  if (err != ESP_OK) {
    Serial0.printf("[print] submit fallo: %s\n", esp_err_to_name(err));
  } else if (xSemaphoreTake(s_xferDone, pdMS_TO_TICKS(TRANSFER_TIMEOUT)) == pdTRUE) {
    Serial0.printf("[print] status=%d actual=%d/%u\n", (int)xfer->status,
                   xfer->actual_num_bytes, (unsigned)len);
  } else {
    Serial0.println("[print] timeout, abortando endpoint");
    usb_host_endpoint_flush(job.dev, job.out_ep);
    xSemaphoreTake(s_xferDone, pdMS_TO_TICKS(1000));
  }

  usb_host_transfer_free(xfer);
  usb_host_interface_release(s_client, job.dev, job.iface);
  xSemaphoreGive(s_printMutex);
}

static void deviceAdded(uint8_t address) {
  usb_device_handle_t dev = NULL;
  if (usb_host_device_open(s_client, address, &dev) != ESP_OK) {
    Serial0.printf("[usb] no se pudo abrir el dispositivo %u\n", address);
    return;
  }

  usb_device_info_t info;
  const usb_device_desc_t *desc = NULL;
  if (usb_host_device_info(dev, &info) == ESP_OK && usb_host_get_device_descriptor(dev, &desc) == ESP_OK) {
    Serial0.printf("[usb] %04X:%04X speed=%d cfg=%u\n",
                   desc->idVendor, desc->idProduct, (int)info.speed, info.bConfigurationValue);
  }

  PrintJob job = {};
  job.dev = dev;
  bool isPrinter = (desc != NULL && desc->idVendor == PRINTER_VID && desc->idProduct == PRINTER_PID);

  if (findBulkOut(dev, job)) {
    Serial0.printf("[usb] endpoint bulk OUT: 0x%02X (iface %u, mps %u)%s\n",
                   job.out_ep, job.iface, job.mps, isPrinter ? "" : "  [VID/PID no coincide]");
    s_lastJob = job;
    s_lastJob.valid = true;
    xQueueSend(s_jobQueue, &job, 0);
  } else {
    Serial0.println("[usb] sin endpoint bulk OUT, no parece impresora");
  }

  for (int i = 0; i < MAX_OPEN_DEVICES; i++) {
    if (s_devices[i] == NULL) {
      s_devices[i] = dev;
      return;
    }
  }
  Serial0.println("[usb] sin ranuras libres, cerrando");
  usb_host_device_close(s_client, dev);
}

static void deviceGone(usb_device_handle_t dev) {
  for (int i = 0; i < MAX_OPEN_DEVICES; i++) {
    if (s_devices[i] == dev) {
      s_devices[i] = NULL;
      break;
    }
  }
  if (s_lastJob.dev == dev) {
    s_lastJob.valid = false;
  }
  usb_host_device_close(s_client, dev);
  Serial0.println("[usb] dispositivo retirado");
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

static void printerTask(void *) {
  PrintJob job;
  while (true) {
    if (xQueueReceive(s_jobQueue, &job, portMAX_DELAY) == pdTRUE) {
      printTest(job);
    }
  }
}

void setup() {
  Serial0.begin(115200);
  delay(800);
  Serial0.println();
  Serial0.println("=== TEST IMPRESORA USB (VID 0416 / PID 5011) ===");

  s_jobQueue   = xQueueCreate(4, sizeof(PrintJob));
  s_xferDone   = xSemaphoreCreateBinary();
  s_printMutex = xSemaphoreCreateMutex();

  USBHostBoardInit();

  usb_host_config_t host_config = {};
  host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
  host_config.enum_filter_cb = enumFilter;
#endif

  esp_err_t err = usb_host_install(&host_config);
  if (err != ESP_OK) {
    Serial0.printf("[usb] usb_host_install fallo: %s\n", esp_err_to_name(err));
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
  xTaskCreate(printerTask, "printer",     4096, NULL, 4, NULL);

  Serial0.println("Host listo. Conecta la impresora al puerto USB-OTG.");
  Serial0.println("Escribi 'p' + Enter para reimprimir el ultimo ticket.");
}

void loop() {
  while (Serial0.available()) {
    int c = Serial0.read();
    if (c == 'p' && s_lastJob.valid) {
      printTest(s_lastJob);
    }
  }
  delay(20);
}
