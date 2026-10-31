/*
 * Prueba 3: hub Atolla con escaner + impresora.
 *
 * Solo enumera y vuelca descriptores de todo lo que cuelgue del hub,
 * clasificando cada dispositivo (hub / impresora / CDC). No envia ni lee
 * datos todavia: sirve para confirmar el hub y saber la clase real del
 * escaner antes de escribir su driver.
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

extern "C" void USBHostBoardInit(void);

#define MAX_OPEN_DEVICES 8

static usb_host_client_handle_t s_client = NULL;
static usb_device_handle_t s_devices[MAX_OPEN_DEVICES];

#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
static bool enumFilter(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
  (void)dev_desc;
  *bConfigurationValue = 1;
  return true;
}
#endif

static const char *className(uint8_t cls) {
  switch (cls) {
    case 0x00: return "definida por interfaz";
    case 0x02: return "CDC (comunicacion)";
    case 0x07: return "PRINTER";
    case 0x09: return "HUB";
    case 0x0A: return "CDC (datos)";
    case 0x03: return "HID";
    case 0x08: return "MSC";
    default:   return "?";
  }
}

static void classifyDevice(usb_device_handle_t dev) {
  const usb_device_desc_t *desc = NULL;
  if (usb_host_get_device_descriptor(dev, &desc) != ESP_OK || desc == NULL) {
    Serial0.println("[dev] sin device descriptor");
    return;
  }

  Serial0.printf("[dev] VID=%04X PID=%04X bcdUSB=%04X class=0x%02X (%s) maxpkt0=%u\n",
                 desc->idVendor, desc->idProduct, desc->bcdUSB,
                 desc->bDeviceClass, className(desc->bDeviceClass), desc->bMaxPacketSize0);

  const usb_config_desc_t *cfg = NULL;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || cfg == NULL) {
    Serial0.println("[dev] sin configuracion activa");
    return;
  }

  Serial0.printf("[cfg] interfaces=%u wTotalLength=%u bmAttributes=0x%02X\n",
                 cfg->bNumInterfaces, cfg->wTotalLength, cfg->bmAttributes);

  int offset = 0;
  const usb_intf_desc_t *curItf = NULL;
  const usb_standard_desc_t *cur = (const usb_standard_desc_t *)cfg;
  bool foundPrinter = false, foundCdcComm = false, foundCdcData = false, foundBulkOut = false, foundBulkIn = false;

  while ((cur = usb_parse_next_descriptor(cur, cfg->wTotalLength, &offset)) != NULL) {
    if (cur->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      curItf = (const usb_intf_desc_t *)cur;
      Serial0.printf("  [itf %u alt %u] class=0x%02X (%s) sub=0x%02X proto=0x%02X eps=%u\n",
                     curItf->bInterfaceNumber, curItf->bAlternateSetting,
                     curItf->bInterfaceClass, className(curItf->bInterfaceClass),
                     curItf->bInterfaceSubClass, curItf->bInterfaceProtocol,
                     curItf->bNumEndpoints);
      if (curItf->bInterfaceClass == 0x07) foundPrinter = true;
      if (curItf->bInterfaceClass == 0x02) foundCdcComm = true;
      if (curItf->bInterfaceClass == 0x0A) foundCdcData = true;
    } else if (cur->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && curItf != NULL) {
      const usb_ep_desc_t *ep = (const usb_ep_desc_t *)cur;
      uint8_t type = ep->bmAttributes & 0x03;
      const char *tn = (type == 0x02) ? "bulk" : (type == 0x03) ? "intr" : (type == 0x01) ? "isoc" : "ctrl";
      Serial0.printf("    [ep] addr=0x%02X %s %s mps=%u\n",
                     ep->bEndpointAddress, (ep->bEndpointAddress & 0x80) ? "IN " : "OUT",
                     tn, (unsigned)(ep->wMaxPacketSize & 0x7FF));
      if (type == 0x02 && !(ep->bEndpointAddress & 0x80)) foundBulkOut = true;
      if (type == 0x02 &&  (ep->bEndpointAddress & 0x80)) foundBulkIn = true;
    }
  }

  const char *kind = foundPrinter ? "IMPRESORA"
                    : (foundCdcComm || foundCdcData) ? "ESCANER CDC"
                    : (desc->bDeviceClass == 0x09) ? "HUB"
                    : "desconocido";
  Serial0.printf("[resumen] %04X:%04X -> %s (bulkOut=%d bulkIn=%d)\n",
                 desc->idVendor, desc->idProduct, kind, foundBulkOut, foundBulkIn);
}

static void deviceAdded(uint8_t address) {
  usb_device_handle_t dev = NULL;
  if (usb_host_device_open(s_client, address, &dev) != ESP_OK) {
    Serial0.printf("[usb] no se pudo abrir el dispositivo %u\n", address);
    return;
  }

  usb_device_info_t info;
  if (usb_host_device_info(dev, &info) == ESP_OK) {
    Serial0.printf("[usb] dir=%u speed=%d cfg=%u\n", address, (int)info.speed, info.bConfigurationValue);
  }

  classifyDevice(dev);

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

void setup() {
  Serial0.begin(115200);
  delay(800);
  Serial0.println();
  Serial0.println("=== TEST HUB: escaner + impresora ===");

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

  xTaskCreate(daemonTask, "usbh_daemon", 4096, NULL, 2, NULL);
  xTaskCreate(clientTask, "usbh_client", 4096, NULL, 3, NULL);

  Serial0.println("Host listo. Enumerando lo que cuelgue del hub...");
}

void loop() {
  delay(1000);
}
