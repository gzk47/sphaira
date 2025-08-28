import struct
import usb.core
import usb.util
import time

# magic number (SPH0) for the script and switch.
MAGIC = 0x53504830

# commands
CMD_QUIT = 0
CMD_OPEN = 1
CMD_EXPORT = 1

# results
RESULT_OK = 0
RESULT_ERROR = 1

# flags
FLAG_NONE = 0
FLAG_STREAM = 1 << 0

# disabled, see usbds.cpp usbDsEndpoint_SetZlt
ENABLE_ZLT = 0

class Usb:
    def __init__(self):
        self.__out_ep = None
        self.__in_ep = None
        self.__packet_size = 0

    def wait_for_connect(self) -> None:
        print("waiting for switch")

        dev = None
        while (dev is None):
            dev = usb.core.find(idVendor=0x057E, idProduct=0x3000)
            if (dev is None):
                time.sleep(0.5)

        print("found the switch!\n")
        cfg = None

        try:
            cfg = dev.get_active_configuration()
            print("found active config")
        except usb.core.USBError:
            print("no currently active config")
            cfg = None

        if cfg is None:
            dev.reset()
            dev.set_configuration()
            cfg = dev.get_active_configuration()

        is_out_ep = lambda ep: usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_OUT
        is_in_ep = lambda ep: usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN
        self.__out_ep = usb.util.find_descriptor(cfg[(0,0)], custom_match=is_out_ep)
        self.__in_ep = usb.util.find_descriptor(cfg[(0,0)], custom_match=is_in_ep)
        assert self.__out_ep is not None
        assert self.__in_ep is not None

        print("iManufacturer: {} iProduct: {} iSerialNumber: {}".format(dev.manufacturer, dev.product, dev.serial_number))
        print("bcdUSB: {} bMaxPacketSize0: {}".format(hex(dev.bcdUSB), dev.bMaxPacketSize0))
        self.__packet_size = 1 << dev.bMaxPacketSize0

    def read(self, size: int, timeout: int = 0) -> bytes:
        if (ENABLE_ZLT and size and (size % self.__packet_size) == 0):
            size += 1
        return self.__in_ep.read(size, timeout)

    def write(self, buf: bytes, timeout: int = 0) -> int:
        return self.__out_ep.write(data=buf, timeout=timeout)

    def get_send_header(self) -> tuple[int, int, int]:
        header = self.read(16)
        [magic, arg2, arg3, arg4] = struct.unpack('<IIII', header)

        if magic != MAGIC:
            raise Exception("Unexpected magic {}".format(magic))

        return arg2, arg3, arg4

    def get_send_data_header(self) -> tuple[int, int, int]:
        header = self.read(16)
        return struct.unpack('<QII', header)

    def send_result(self, result: int, arg3: int = 0, arg4: int = 0) -> None:
        send_data = struct.pack('<IIII', MAGIC, result, arg3, arg4)
        self.write(send_data)
