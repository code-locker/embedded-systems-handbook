# 07 — GPIO, I2C and SPI Drivers

> The three buses every embedded board is full of. This lesson shows how
> Linux splits each of them into **controller** drivers (written once per
> SoC, by TI for the TDA4VM) and **client** drivers (written by you, once
> per chip), and then writes the client side four times: a GPIO consumer
> with an interrupt, a 24C02-style EEPROM with raw I2C transfers and
> nvmem, a regmap sensor on I2C, and the same sensor on SPI. Each one
> can be tested without hardware (i2c-stub, gpio-sim in a VM) and on the
> BeagleBone AI-64 header with real parts and device-tree overlays. The
> user-space side, `/dev/gpiochipN`, `/dev/i2c-N` and `/dev/spidevB.C`,
> gets a raw-ioctl program each.

---

## 1. Why this exists

Almost every chip on an embedded board that is not the SoC itself is
reached through one of three buses:

- **GPIO**: one wire per signal: LEDs, buttons, reset and enable pins,
  interrupt lines of other chips.
- **I2C**: two wires shared by many slow chips: EEPROMs, sensors, PMICs,
  camera control interfaces (every CSI-2 camera sensor on the AI-64 is
  configured over I2C), HDMI DDC, RTCs.
- **SPI**: four wires, faster and full duplex: ADCs, displays, flash,
  CAN controllers, radio modules.

You could poke the controller registers yourself (`/dev/mem`), but then
every program has to know the SoC, two programs can fight over one bus,
and nothing handles the interrupt. Linux solves this with one
**subsystem per bus**: gpiolib, the I2C core and the SPI core. Each one
sits between the drivers for the *controller* (the SoC block) and the
drivers for the *devices* on the bus. A sensor driver written against
the I2C core works unchanged on a TDA4VM, a Raspberry Pi and a PC's
SMBus.

The questions this lesson answers: how does my driver get "its" GPIO,
I2C address or chip select without hard-coding it? How do I talk to the
chip correctly (repeated start, page writes, SPI modes, DMA-safe
buffers)? How do I test it without the hardware? And when is user space
(`libgpiod`, `i2c-tools`, `spidev`) the better choice than a driver?

---

## 2. Mental model

**A bus is a building's mail system.** The *controller driver* is the
post office: it knows how to move letters (clock bits on SCL/SDA,
SCLK/MOSI/MISO, set a pad high). The *client driver* is a tenant who
knows what the letters mean ("register 0x20 bit 0 = enable"). The
*device tree* is the address book: which tenant lives behind which door
(I2C address 0x50, SPI chip select 0, GPIO line 4). The bus core is the
sorting office: it reads the address book, creates a "device" for every
entry, and introduces each one to the tenant (driver) whose name matches.

```
              user space:  gpioset   i2cget    your app     spidev_test
                             |          |         |            |
              ------------ /dev/gpiochipN  /dev/i2c-N  sysfs  /dev/spidevB.C ------
                             |          |         |            |
 client (device) drivers:  [gpio cdev] [i2c-dev] [esh_sensor] [spidev] [esh_spi_sensor]
                             |          |         |            |          |
 bus core:                 gpiolib     I2C core (i2c_client)  SPI core (spi_device)
                             |          |                      |
 controller drivers:       gpio-davinci   i2c-omap               spi-omap2-mcspi
 (TI, per SoC)               |          |                      |
 hardware:                 GPIO block  I2C block              McSPI block
                             |          |                      |
                           pads ------ pinmux (pinctrl-single) ------ header pins P8/P9
```

The sentence to remember: **you write the client driver; you ask the
core for *your* resource by function name or by match table, and the
device tree says where it really is.**

---

## 3. How it works

### 3.0 Controllers vs clients, once per bus

| | GPIO | I2C | SPI |
|---|---|---|---|
| controller object | `struct gpio_chip` / `gpio_device` | `struct i2c_adapter` (+ `i2c_algorithm`) | `struct spi_controller` |
| TDA4VM controller driver | `gpio-davinci` (main_gpio0..7, wkup_gpio0/1) | `i2c-omap` (main_i2c0..6, wkup/mcu_i2c) | `spi-omap2-mcspi` (main_spi0..7, mcu_spi) |
| device object | a line (`struct gpio_desc`) | `struct i2c_client` (adapter + 7-bit address) | `struct spi_device` (controller + chip select + mode + speed) |
| client driver | any driver: `devm_gpiod_get()` | `struct i2c_driver` | `struct spi_driver` |
| match by | function name (`"led"` -> `led-gpios`) | `of_match_table`, `i2c_device_id` | `of_match_table`, `spi_device_id` |
| user space | `/dev/gpiochipN` (GPIO cdev), libgpiod | `/dev/i2c-N` (i2c-dev), i2c-tools | `/dev/spidevB.C` (spidev) |
| simulator for a VM | `gpio-sim` (configfs) | `i2c-stub` | none in mainline |

I2C, as the I2C core sees it:

```
  i2c_adapter "OMAP I2C adapter" (i2c-2 = main_i2c2, P9.19/P9.20)
     |-- i2c_client 2-0050  name "24c02"  -> bound to i2c_driver esh_eeprom
     |-- i2c_client 2-0048  name "temp-sensor" -> bound to esh_sensor
     '-- (address 0x3c: no client; i2cdetect may still see a chip there)
```

SPI:

```
  spi_controller spi7 (main_spi7, P9.28-P9.31)
     |-- spi_device spi7.0  (CS0, mode 0, 1 MHz)  -> esh_spi_sensor or spidev
     '-- spi_device spi7.1  (CS1, if ti,spi-num-cs >= 2 and pinned out)
```

GPIO:

```
  gpio_device gpiochipN = main_gpio0 (label "600000.gpio", N varies), lines 0..127
     |-- line 4  "(AJ23)P8_18"  consumer "esh-led"     <- esh_gpio_demo
     '-- line 3  "(AF22)P8_17"  consumer "esh-button"  <- esh_gpio_demo
```

How a device gets created and bound (details of the device model and
overlays are in [06-device-model-and-device-tree](../06-device-model-and-device-tree/)):
the controller driver probes, registers its adapter/controller, and the
core walks the controller's DT child nodes, creating one `i2c_client` /
`spi_device` per child. Each new device is matched against all
registered drivers of that bus; on a match the driver's `.probe` runs.

### 3.1 GPIO: gpiolib and the descriptor API  (`kernel/esh_gpio_demo.c`)

**Consumer API** (`<linux/gpio/consumer.h>`):

| Call | Meaning |
|---|---|
| `devm_gpiod_get(dev, "led", GPIOD_OUT_LOW)` | look up `led-gpios` in DT (or a lookup table), request it, make it an output at logical 0, release on unbind |
| `devm_gpiod_get_optional(dev, "button", GPIOD_IN)` | same, but `NULL` (not an error) if the property is absent |
| `devm_gpiod_get_index(dev, "data", 2, ...)` / `devm_gpiod_get_array(dev, "data", ...)` | for `data-gpios = <...>, <...>, ...` (a parallel bus); `gpiod_set_array_value()` sets them in one call |
| `gpiod_set_value(desc, v)` / `gpiod_get_value(desc)` | **atomic context OK**, only for chips that do not sleep (SoC GPIO) |
| `gpiod_set_value_cansleep()` / `gpiod_get_value_cansleep()` | process context only; works for every chip, also GPIO expanders on I2C/SPI |
| `gpiod_cansleep(desc)` | does this chip sleep? (expander: yes) |
| `gpiod_direction_input/output()` | change direction later |
| `gpiod_to_irq(desc)` | Linux IRQ number for the line (if the controller can interrupt) |
| `gpiod_set_consumer_name()` | label shown by `gpioinfo` |

**Values are logical.** `1` means "active". The DT flag decides what
active means electrically:

```
  led-gpios    = <&main_gpio0 4 GPIO_ACTIVE_HIGH>;   logical 1 -> pad high
  button-gpios = <&main_gpio0 3 GPIO_ACTIVE_LOW>;    pad low (pressed to GND) -> logical 1
```

so the driver never contains `!` for wiring inversions. Other flags:
`GPIO_OPEN_DRAIN`, `GPIO_PULL_UP` (if the controller supports bias).

**The legacy integer API** (`gpio_request(412)`, `gpio_set_value()`,
`of_get_named_gpio()`, `<linux/gpio.h>`) is deprecated: global numbers
change from boot to boot, active-low must be handled by hand, and nothing
is released automatically. New code uses descriptors only.

**Interrupts from a GPIO**: `gpiod_to_irq()` + `devm_request_threaded_irq()`.
`esh_gpio_demo` passes no hard-IRQ handler, only a thread function, plus
`IRQF_ONESHOT` (the line stays masked until the thread is done). The thread
may call `gpiod_get_value_cansleep()`. On the board's PREEMPT_RT kernel
every handler is threaded anyway (lesson [03](../03-kernel-synchronization/)
and [04](../04-interrupts-timers-and-deferred-work/)). Debouncing a
mechanical button: `gpiod_set_debounce()` if the controller supports it,
else a timer.

**DT for the demo on the board** (P8.18 = main_gpio0 line 4 for the
LED, P8.17 = line 3 for the button; the line names in the board's
`k3-j721e-beagleboneai64-pinmux.dtsi` say so). Check them with
`gpioinfo` before wiring anything. The pad offsets below follow the
pattern of the board's own `-io-pins` overlay (pad 0x10 is P8.18) but
were **not** checked against the schematic:

```dts
&main_pmx0 {
	esh_gpio_pins: esh-gpio-pins {
		pinctrl-single,pins = <
			J721E_IOPAD(0x10, PIN_INPUT, 7)  /* P8.18 gpio0_4: input buffer ON so the LED can be read back */
			J721E_IOPAD(0x0c, PIN_INPUT, 7)  /* P8.17 gpio0_3 (verify offset) */
		>;
	};
};
&{/} {
	esh-gpio-demo {
		compatible = "esh,gpio-demo";
		pinctrl-names = "default";
		pinctrl-0 = <&esh_gpio_pins>;
		led-gpios = <&main_gpio0 4 GPIO_ACTIVE_HIGH>;
		button-gpios = <&main_gpio0 3 GPIO_ACTIVE_LOW>;
	};
};
```

**User space: the GPIO character device.** `/dev/gpiochipN` per chip
(since 4.8, v2 uAPI since 5.10). `/sys/class/gpio` (export/unexport)
is **deprecated**: global numbers, no owner, a crashed program leaves the
line configured. With the cdev a line belongs to the file descriptor that
requested it and goes back when that fd closes.

```sh
gpiodetect                         # gpiochip0 [600000.gpio] (128 lines) ...
gpioinfo gpiochip1 | head          # names, consumers, direction
gpioget  -c gpiochip1 3            # libgpiod v2 syntax; v1 (Ubuntu 22.04): gpioget gpiochip1 3
gpioset  -c gpiochip1 4=1          # v2 keeps the line until Ctrl-C; v1 releases it on exit
gpiomon  -c gpiochip1 3            # edge events with timestamps
```

`user/gpio_cdev_read.c` does the same with the raw ioctls:

```
 open("/dev/gpiochipN") --GPIO_GET_CHIPINFO_IOCTL--> name, label, lines
        |              --GPIO_V2_GET_LINEINFO_IOCTL (per offset)--> name, consumer, flags
        |              --GPIO_V2_GET_LINE_IOCTL {offsets, flags, consumer}--> req.fd  (NEW fd)
        |                                                                  |
        |              GPIO_V2_LINE_GET_VALUES_IOCTL {mask} <--------------+  bits
        |              poll()/read(req.fd) -> struct gpio_v2_line_event   (edge, ts, seqno)
        '-- close(req.fd): lines released
```

**Pinctrl / pinmux on the TDA4VM.** One SoC ball can carry up to ~8
functions (I2C2_SCL, MCAN0_RX, GPIO1_1, ...). A per-pad register in the
`main_pmx0` block (`pinctrl@11c000`, driver `pinctrl-single`) selects the
function and the electrical setup:

```
  pad register (32 bit), J721E_IOPAD(offset, flags, mode) in k3-pinctrl.h
   bit 21  TX_DISABLE   PIN_DISABLE = output driver off ("parked" pad)
   bit 18  RXACTIVE     input buffer on  (PIN_INPUT*), needed to READ the pad
   bit 17  PULLTYPESEL  1 = pull-up
   bit 16  PULLUDEN     1 = pull DISABLED (note the inversion)
   bit 14  ST_EN        Schmitt trigger
   bits 3:0 MUXMODE     0..7; 7 = GPIO on most pads
```

A driver's DT node lists `pinctrl-names = "default"; pinctrl-0 = <&grp>;`
and the device core applies that group **before** `.probe` runs. Two
things are special on the BeagleBone AI-64:

- Most header pins are wired to **two** SoC balls (the "a" and "b" names
  in `gpioinfo`: `P9_19a` = ball W5, `P9_19b` = ball AF29). The unused
  ball must be parked (`PIN_DISABLE` or a harmless input), otherwise both
  drive the pin. The board DTS does exactly that for `main_i2c2`: it muxes
  W5/W6 to I2C2 and sets AE25/AF29 to plain GPIO inputs.
- Many header pins already have a default function in the board DTS (I2C
  on P9.17/18, P9.19/20, P9.24/26). Reusing such a pin for SPI means
  disabling that bus in the overlay. The vendor SPI6 overlay does that:
  `&main_i2c6 { status = "disable"; }`.

Debug: `sudo cat /sys/kernel/debug/pinctrl/11c000.pinctrl-pinctrl-single/pinmux-pins`
shows which device owns which pad. The bit layout above is the same
register-field arithmetic as [dsa/18-bit-manipulation](../../dsa/18-bit-manipulation/).

### 3.2 I2C protocol recap

```
 SCL ‾‾‾‾\_/‾\_/‾\_ ... _/‾\_/‾\_/‾\_ ... _/‾‾‾‾‾      open drain: devices only
 SDA ‾‾\___X___X___ ... __X___/‾‾‾X__ ... ___/‾‾      pull LOW; pull-up resistors
       S  A6  A5     A0 R/W ACK  D7          P        make the HIGH level

 S  = START: SDA falls while SCL high      P = STOP: SDA rises while SCL high
 address byte = 7-bit address << 1 | R/W (0 = write, 1 = read)
 ACK = receiver pulls SDA low in the 9th clock; NACK = leaves it high
 Sr = repeated START: a new START without a STOP, so the bus is not released
```

A register read is two messages glued by a repeated start:

```
 S | 0x48 W | A | reg 0x0F | A | Sr | 0x48 R | A | data | NA | P
   '------ write: set pointer ------'  '---- read: get value ----'
```

Speeds: 100 kHz (standard), 400 kHz (fast), 1 MHz (fast-plus). The
header buses of the AI-64 run at 100 kHz (`clock-frequency = <100000>`
in the board DTS); camera buses run at 400 kHz. Clock stretching: a
slow slave may hold SCL low; the master must wait.

**SMBus** is a stricter subset (timeouts, 10–100 kHz, defined
transactions): *quick*, *byte*, *byte data* (one register), *word data*
(16-bit little endian), *block data* (length byte first, max 32), and the
non-SMBus but widely emulated *I2C block* (no length byte). PC chipsets
and `i2c-stub` speak only SMBus, a TDA4VM controller speaks raw I2C,
which can emulate every SMBus transaction. That is why drivers check
**what the adapter can do** first:

```c
if (i2c_check_functionality(adap, I2C_FUNC_I2C))                 /* raw */
else if (i2c_check_functionality(adap, I2C_FUNC_SMBUS_READ_I2C_BLOCK))
else if (i2c_check_functionality(adap, I2C_FUNC_SMBUS_READ_BYTE_DATA))
```

### 3.3 I2C client drivers  (`kernel/esh_eeprom.c`, `kernel/esh_sensor.c`)

```c
static const struct of_device_id my_of_match[] = {
	{ .compatible = "esh,24c02" }, { }               /* DT */
};
static const struct i2c_device_id my_id[] = {
	{ "esh_eeprom", 0 }, { }                           /* new_device, board files */
};
static struct i2c_driver my_driver = {
	.driver   = { .name = "esh_eeprom", .of_match_table = my_of_match },
	.probe    = my_probe,          /* int (*)(struct i2c_client *), single arg since 6.3 */
	.remove   = my_remove,         /* void, since 6.1; often not needed (devm_) */
	.id_table = my_id,
};
module_i2c_driver(my_driver);    /* or i2c_add_driver()/i2c_del_driver() */
```

Transfers, from simplest to most general (all **sleep**: process context
only, never under a spinlock or in a hard IRQ handler):

| Call | Wire | Returns |
|---|---|---|
| `i2c_smbus_read_byte_data(client, reg)` | S addr W reg Sr addr R data P | `0..255` **or `-errno`**: store it in an `int` |
| `i2c_smbus_write_byte_data(client, reg, v)` | S addr W reg v P | 0 / `-errno` |
| `i2c_smbus_read_word_data()` | ... 2 bytes, little endian | `0..65535` / `-errno` (`_swapped` for big-endian chips) |
| `i2c_smbus_read_i2c_block_data(client, reg, n, buf)` | ... n ≤ 32 bytes | bytes read / `-errno` |
| `i2c_master_send()` / `i2c_master_recv()` | one write / one read message | bytes / `-errno` |
| `i2c_transfer(adap, msgs, n)` | any sequence, `Sr` between messages, one `P` at the end | number of messages done / `-errno` |

`struct i2c_msg { addr, flags (I2C_M_RD, I2C_M_TEN, I2C_M_NOSTART...), len, buf }`.
`esh_eeprom.c` builds the EEPROM "random read" as two `i2c_msg`s in **one**
`i2c_transfer()` call: if you sent them as two calls another master
(or another process using i2c-dev) could move the address pointer in
between.

EEPROM specifics shown in `esh_eeprom.c`: page writes must not cross a
page boundary (the internal address counter wraps within the page), and
after a write the chip NACKs for ~5 ms while programming. The driver
retries until it ACKs again ("ACK polling") instead of sleeping a fixed
time. The data reaches user space through **nvmem**
(`devm_nvmem_register`), the framework the real `at24` driver uses, so
you get `/sys/bus/nvmem/devices/esh_eeprom0/nvmem` and in-kernel
consumers (a MAC address cell, say) for free.

**Instantiating an I2C device** (there is no enumeration on I2C, so
something has to say "a 24c02 lives at 0x50"):

1. **Device tree** child node under the controller (`kernel/dts/esh-i2c2-devices.dtso`):
   ```dts
   &main_i2c2 { eeprom@50 { compatible = "esh,24c02"; reg = <0x50>; }; };
   ```
2. **From user space**, for testing:
   ```sh
   echo esh_eeprom 0x50 | sudo tee /sys/bus/i2c/devices/i2c-2/new_device
   echo 0x50            | sudo tee /sys/bus/i2c/devices/i2c-2/delete_device
   ```
   The name must match an `i2c_device_id` entry.
3. Board files / ACPI / `i2c_new_client_device()` from another driver
   (a multi-function chip's driver creating its sub-devices).

**User space: i2c-dev.** `/dev/i2c-N`, `ioctl(fd, I2C_SLAVE, addr)` to
pick the target (EBUSY if a kernel driver owns that address), then
`ioctl(I2C_SMBUS)` or `ioctl(I2C_RDWR, struct i2c_rdwr_ioctl_data)`.
`user/i2c_smbus_rw.c` uses `I2C_SMBUS` directly. The tools:

```sh
i2cdetect -l                 # list adapters (i2c-2 = main_i2c2 on the board)
sudo i2cdetect -y -r 2       # scan: "--" nothing, "UU" a driver owns it, "50" chip present
sudo i2cget -y 2 0x50 0x00   # read register 0
sudo i2cset -y 2 0x50 0x10 0xa5
sudo i2cdump -y 2 0x50       # 256 registers
```

`i2cdetect` probes by writing or reading: on some chips (write-only
devices, some EEPROMs with write-protect off) a probe write can change
state. Never scan buses you do not know (a PC's SMBus holds DIMM SPD
EEPROMs).

### 3.4 regmap  (`kernel/esh_sensor.c`)

Most sensor/PMIC/codec drivers describe registers once and let regmap
do the bus work:

```c
static const struct regmap_config cfg = {
	.reg_bits = 8, .val_bits = 8, .max_register = 0x29,
	.readable_reg = ..., .writeable_reg = ..., .volatile_reg = ...,
	.cache_type = REGCACHE_MAPLE,           /* REGCACHE_RBTREE before 6.4 */
};
map = devm_regmap_init_i2c(client, &cfg);   /* or devm_regmap_init_spi(spi, &cfg) */
regmap_read(map, REG, &val);
regmap_update_bits(map, CTRL, EN | RATE, EN | FIELD_PREP(RATE, 3));
regmap_bulk_read(map, DATA_L, buf, 2);      /* one transfer, auto-increment */
```

What you get: the bus transfer chosen for the adapter (regmap-i2c uses
raw I2C, I2C block or byte SMBus as available), locking, a **register
cache** (non-volatile registers are read from RAM; `regmap_update_bits`
on a cached register costs one bus write instead of read+write; after
suspend `regcache_sync()` restores everything), range checks, tracepoints
and `/sys/kernel/debug/regmap/<dev>/registers`. Volatile registers
(status, data: the chip changes them on its own) always go to the bus.

The fictional sensor of this lesson:

| Reg | Name | Access | Content |
|---|---|---|---|
| 0x0F | WHO_AM_I | RO | always `0xE5`; checked in probe before anything is written |
| 0x20 | CTRL | RW, cached | bit 0 EN, bits 3:1 RATE |
| 0x27 | STATUS | RO, volatile | bit 0 DRDY |
| 0x28/0x29 | DATA_L/H | RO, volatile | signed 16-bit little endian, 0.01 °C |

With `i2c-stub` you play the chip: `i2cset` the values the driver will
read (section 8).

### 3.5 SPI  (`kernel/esh_spi_sensor.c`, `user/spidev_loopback.c`)

```
 CS   ‾‾\______________________________________/‾‾   active low, one per device
 SCLK ____/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\________    mode 0: idle low,
 MOSI ====X=7=X=6=X=5=X=4=X=3=X=2=X=1=X=0=X=====      sample on rising edge
 MISO ====X=7=X=6=X=5=X=4=X=3=X=2=X=1=X=0=X=====      both directions AT ONCE

 mode  CPOL CPHA   clock idles   data sampled on
  0     0    0       low          rising (first) edge
  1     0    1       low          falling (second) edge
  2     1    0       high         falling (first) edge
  3     1    1       high         rising (second) edge
```

No addresses, no ACK: the chip select picks the device, and every
transfer is full duplex (to read n bytes you clock out n bytes, often
dummies). Speeds are MHz to tens of MHz; the TDA4VM McSPI reference clock is 48 MHz.

Kernel objects: an **`spi_transfer`** is one buffer pair (`tx_buf`,
`rx_buf`, `len`, optional `speed_hz`, `bits_per_word`, `cs_change`,
delays); an **`spi_message`** is a list of transfers run with CS held
asserted. `spi_sync(spi, &msg)` sleeps until done; `spi_async()` calls
`msg->complete` from the controller's context. Helpers:
`spi_write()`, `spi_read()`, `spi_write_then_read(spi, tx, ntx, rx, nrx)`
(half duplex, copies through an internal DMA-safe buffer, so stack
buffers are fine **only** for this helper).

**DMA-safe buffers.** Buffers passed in `spi_transfer` may go straight to
the DMA engine (McSPI uses DMA above a size threshold), so they must come
from `kmalloc`/`devm_kzalloc`, never the stack or `vmalloc` (lesson
[08](../08-dma-and-mmap/) explains why).

The driver:

```c
static struct spi_driver drv = {
	.driver   = { .name = "esh_spi_sensor", .of_match_table = of_ids },
	.probe    = probe,               /* int (*)(struct spi_device *) */
	.remove   = remove,              /* void since 5.18; optional with devm_ */
	.id_table = spi_ids,             /* "spi-temp-sensor": the compatible without vendor */
};
```

The SPI core warns at registration if a DT compatible has no matching
`spi_device_id` entry, because module autoloading uses `spi:<name>`.
Mode/speed come from DT (`spi-max-frequency`, `spi-cpol`, `spi-cpha`,
`spi-cs-high`); the driver may tighten them and must call `spi_setup()`.
regmap-spi with `read_flag_mask = 0x80` implements the usual sensor
convention "address byte bit 7 = read".

**spidev**: a generic SPI client driver that exposes
`/dev/spidevB.C`. Writing `compatible = "spidev"` in DT is refused by
the driver ("spidev listed directly in DT is not supported"): DT
describes **hardware**, and "spidev" is not a chip. Options: a
compatible from spidev's own table (BeagleBoard's overlays use
`"rohm,dh2228fv"` for that reason), or bind any SPI device to spidev at
run time:

```sh
D=spi7.0                                     # ls /sys/bus/spi/devices/
echo $D     | sudo tee /sys/bus/spi/devices/$D/driver/unbind     # if a driver holds it
echo spidev | sudo tee /sys/bus/spi/devices/$D/driver_override
sudo modprobe spidev
echo $D     | sudo tee /sys/bus/spi/drivers/spidev/bind          # -> /dev/spidev7.0
```

User space: `ioctl(SPI_IOC_WR_MODE32 / _BITS_PER_WORD / _MAX_SPEED_HZ)`,
then `ioctl(fd, SPI_IOC_MESSAGE(n), struct spi_ioc_transfer[n])`;
pointers travel as `__u64`, unused fields must be zero.

**Loopback testing**: a jumper from MOSI to MISO makes rx == tx, which
tests pinmux, clocking and the whole software stack without any chip.
`esh_spi_sensor.ko loopback=1` does it in the kernel,
`user/spidev_loopback.c` from user space. The kernel also has
`spi-loopback-test` (`CONFIG_SPI_LOOPBACK_TEST`, not enabled in the
board's config), a suite of transfer patterns to run against a looped
controller.

### 3.6 Device tree for I2C and SPI devices on the AI-64

From the board DTS (`k3-j721e-beagleboneai64.dts` + `-pinmux.dtsi`):

| Header pins | Controller | Label(s) | Linux | Default |
|---|---|---|---|---|
| P9.19 SCL / P9.20 SDA | `main_i2c2` (i2c@2020000) | `bone_i2c_2` | `/dev/i2c-2` (alias) | enabled, 100 kHz |
| P9.17 SCL / P9.18 SDA | `main_i2c6` | `bone_i2c_1` | `/dev/i2c-1` | enabled, 100 kHz |
| P9.24 SCL / P9.26 SDA | `main_i2c4` | `bone_i2c_3` | `/dev/i2c-3` | enabled, 100 kHz |
| P9.28 CS0, P9.31 SCLK, P9.30 MOSI, P9.29 MISO | `main_spi7` (spi@2170000) | | `spiN.0` (dynamic number) | disabled, pads not muxed |
| P9.17/P9.22/P9.21/P9.18 | `main_spi6` | | | disabled; conflicts with `main_i2c6` |

`kernel/dts/esh-i2c2-devices.dtso` only adds child nodes to the already
enabled `main_i2c2`. `kernel/dts/esh-spi7-device.dtso` has to mux four
pads to mode 6, park the "b" twins, enable `main_spi7` with
`ti,pindir-d0-out-d1-in` (McSPI's data lines are bidirectional; without
it D0 is the input and MISO stays silent) and add the device. Its pad data
is copied from BeagleBoard's `k3-j721e-beagleboneai64-spi-mcspi7-cs0.dtso`
in the board kernel tree; it was **not** verified against the schematic.
The overlay comment says how to check it on the board.

Electrical: 3.3 V only; I2C needs real pull-ups (~4.7 kΩ, usually on the
breakout board); short wires for SPI at MHz rates.

### 3.7 Other buses, briefly

| Bus | Kernel framework | Client driver struct | User space | On the TDA4VM / AI-64 |
|---|---|---|---|---|
| UART | serial core + **serdev** (a driver for a chip on a UART: GPS, BT) | `struct serdev_device_driver` | `/dev/ttyS*`, termios | main_uart0 = console; more on the header via overlays |
| 1-Wire | w1 | `struct w1_family_ops` | `/sys/bus/w1/devices/` | bit-banged on a GPIO (`w1-gpio`) |
| CAN | SocketCAN (netdev) | — (controller drivers: `m_can`) | `ip link set can0 up type can bitrate 500000`, `candump` | MCAN controllers; header CAN needs a transceiver |
| USB | USB core | `struct usb_driver` (+ gadget side) | libusb, `/dev/bus/usb` | USB 3.0 host + Type-C (cdns3) |
| PCIe | PCI core | `struct pci_driver` | `lspci`, sysfs `resource*` | M.2 Key E slot (pcie1_rc) |

The future `protocols/` track goes into the wire-level details.

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| I2C byte register read @100 kHz | ~0.4 ms | 2 address bytes + reg + data = 4 × 9 clocks + S/Sr/P ≈ 40 clocks at 10 µs |
| I2C byte register read @400 kHz | ~0.1 ms | same 40 clocks |
| 24C02 page write (8 bytes) | ~1 ms bus + **5 ms** tWR | programming time dominates: write in pages, not bytes |
| `regmap_update_bits` on a cached register | 1 bus write | the read comes from the cache |
| `regmap_update_bits`, no cache / volatile | 1 read + 1 write | read-modify-write on the bus |
| SPI transfer n bytes @ f Hz | 8n / f + setup | full duplex: reading costs the same clocks as writing |
| `gpiod_set_value` (SoC GPIO) | one MMIO write, ~100 ns | atomic context safe |
| `gpiod_set_value_cansleep` on an I2C expander | one I2C transaction, ~0.3 ms | why `_cansleep` exists |
| GPIO IRQ to threaded handler | µs to tens of µs | thread wakeup; RT priority on PREEMPT_RT |
| `i2c_transfer` from user space (i2c-dev) | syscall + the same bus time | bus time dominates; the ioctl is cheap |

---

## 5. C vs C++ — and kernel vs user space

The kernel is C only; the comparison that matters is **driver vs user
space**:

| | Kernel client driver | User space (libgpiod / i2c-dev / spidev) |
|---|---|---|
| Bus access | `i2c_transfer`, `spi_sync`, `gpiod_*`, regmap | `ioctl(I2C_SMBUS)`, `SPI_IOC_MESSAGE`, GPIO v2 ioctls |
| Interrupts | `gpiod_to_irq` + threaded handler, µs latency | `gpiomon`/`poll()` on line events; no access to a chip's IRQ register logic |
| Exposes a standard interface | yes: hwmon, IIO, input, nvmem, net, LED class | no: every app invents its own |
| Concurrency with other users | bus locks, one owner per address | `I2C_SLAVE` EBUSY protects bound addresses only |
| Crash consequences | kernel oops | process dies; GPIO lines released automatically |
| When to choose | other drivers or many apps need the chip, needs IRQs, power management | prototyping, one app owns the chip, fast iteration |

C++ in user space: libgpiod v2 ships C++ bindings (`gpiod::chip`,
`gpiod::line_request`, RAII release). For I2C/SPI wrap the fd in a class
with a destructor that closes it, and `std::span<std::byte>` for buffers.

---

## 6. Where it is used in the real world

- **at24** (`drivers/misc/eeprom/at24.c`): the real version of
  `esh_eeprom`. The AI-64's board-ID EEPROM (`atmel,24c04` at 0x50 on
  `wkup_i2c0`) uses it; U-Boot reads the same EEPROM to pick the DT.
- **Camera sensors** (`drivers/media/i2c/imx219.c`, `ov5640.c`): I2C
  client + regmap (or `cci_*` helpers) for configuration, data over CSI-2;
  the GStreamer camera pipelines of [gstreamer/14-embedded-tda4vm](../../gstreamer/14-embedded-tda4vm/)
  depend on these drivers.
- **IIO** (`drivers/iio/`): ADCs, accelerometers, gyros on I2C/SPI, almost
  all regmap-based; `bmi160`, `adxl345` have `_i2c.c` and `_spi.c`
  front-ends sharing one `_core.c`, the structure `esh_sensor`/`esh_spi_sensor` hint at.
- **hwmon** (`drivers/hwmon/lm75.c`, `tmp102.c`): temperature sensors, the
  standard home for a real `esh_sensor`.
- **gpio-keys / leds-gpio**: generic DT-only drivers that do what
  `esh_gpio_demo` does for real: you write DT, no C.
- **GPIO expanders** (`gpio-pca953x`): a `gpio_chip` provider on an I2C
  client, which is why `_cansleep` exists.
- **mcp251x / tcan4x5x**: SPI CAN controllers exposing SocketCAN.
- **spi-nor / mtd**: SPI flash; **fbtft / tinydrm**: SPI displays.

---

## 7. Common mistakes and pitfalls

1. **I2C/SPI transfer in atomic context** (under a spinlock, in a hard
   IRQ handler, in a timer). Transfers sleep. Use a threaded IRQ handler
   or a workqueue (`kernel/esh_i2c_bug.c bug=1`).
2. **`u8 v = i2c_smbus_read_byte_data(...)`**: `-ENXIO` becomes `0xFA` and
   the driver reports garbage (`esh_i2c_bug.c bug=2`). Keep the `int`,
   check `< 0`.
3. **Two `i2c_transfer()` calls for one register read**: someone else can
   use the bus in between. One call with two `i2c_msg`s.
4. **Writing to a chip before identifying it** (WHO_AM_I / ID register).
   Wrong address, wrong bus, wrong overlay: you misconfigure a foreign chip.
5. **EEPROM page-boundary wrap** and **no write-cycle wait**: the next
   write is NACKed or the data is lost.
6. **Stack buffers in `spi_transfer`**: not DMA-safe. kmalloc them.
7. **`compatible = "spidev"`**: refused. Use a real compatible or
   `driver_override`.
8. **Missing `spi_device_id` / `i2c_device_id`**: module autoload fails or
   the core warns; DT-only drivers still need it for modalias.
9. **Pin conflicts on the AI-64**: forgetting to park the "b" twin of a
   header pin, or enabling SPI6 while `main_i2c6` still owns P9.17/P9.18.
   Read `pinmux-pins` in debugfs.
10. **`gpiod_set_value()` (non-cansleep) on an expander line**: gpiolib
    WARNs; use `_cansleep` in process context.
11. **Inverting active-low in code** (`!value`) *and* in DT: the double
    inversion cancels; let the DT flag do it.
12. **`PIN_OUTPUT` on a GPIO pad you also read**: the input buffer is off,
    `gpiod_get_value()` returns 0. Use `PIN_INPUT` (the TDA4VM pad keeps
    the output driver on regardless when the GPIO is an output).
13. **No pull-ups on I2C**: SDA/SCL never go high, `i2cdetect` shows
    everything or nothing, transfers time out.
14. **Module parameter named like a kernel function**: `page_size` is an
    inline in `<linux/mm.h>`; `esh_eeprom.c` uses `module_param_named()`.
15. **User space and a driver on the same chip**: `I2C_SLAVE` returns
    EBUSY. `I2C_SLAVE_FORCE`/`i2cget -f` work around it, and then the
    driver's cache and state are wrong.

---

## 8. Files in this lesson

| File | What it shows | Build / run |
|---|---|---|
| [kernel/Makefile](kernel/Makefile) | Kbuild makefile: `KDIR=`, `ARCH=`/`CROSS_COMPILE=`, `BUGS=1`, `W=1`, `clean` | `make drivers` |
| [kernel/esh_gpio_demo.c](kernel/esh_gpio_demo.c) | platform driver, `devm_gpiod_get(_optional)`, logical values/active-low, `gpiod_to_irq` + threaded IRQ, sysfs `led`/`button`/`presses`; `sim_chip=` mode with a `gpiod_lookup_table` for gpio-sim | `insmod esh_gpio_demo.ko sim_chip=esh-sim` |
| [kernel/esh_eeprom.c](kernel/esh_eeprom.c) | `i2c_driver`, `i2c_check_functionality` → raw `i2c_transfer` with repeated start / SMBus I2C-block / byte, page writes, ACK polling, nvmem export, read-only self-test | `insmod esh_eeprom.ko` + `new_device` |
| [kernel/esh_sensor.c](kernel/esh_sensor.c) | regmap-i2c: access tables, volatile regs, maple cache, `regmap_update_bits`, `FIELD_PREP`, cache-bypass self-test, devm power-off action, `dev_groups` sysfs | `insmod esh_sensor.ko rate=3` + `new_device` |
| [kernel/esh_spi_sensor.c](kernel/esh_spi_sensor.c) | `spi_driver`, `spi_setup`, regmap-spi with `read_flag_mask`, `spi_message`/`spi_transfer` + `spi_sync`, `spi_write_then_read`, MOSI–MISO loopback self-test | board: `insmod esh_spi_sensor.ko loopback=1` |
| [kernel/esh_i2c_bug.c](kernel/esh_i2c_bug.c) | **intentionally broken** (only with `BUGS=1`): transfer under a spinlock; errno stored in a `u8` | `make -C …/kernel BUGS=1`, `insmod esh_i2c_bug.ko bug=2` |
| [kernel/dts/esh-i2c2-devices.dtso](kernel/dts/esh-i2c2-devices.dtso) | overlay: EEPROM @0x50 + sensor @0x48 on `main_i2c2` (P9.19/P9.20) | see "dtc" below |
| [kernel/dts/esh-spi7-device.dtso](kernel/dts/esh-spi7-device.dtso) | overlay: pinmux + enable `main_spi7` (P9.28–P9.31) + sensor on CS0 | see "dtc" below |
| [user/gpio_cdev_read.c](user/gpio_cdev_read.c) | GPIO v2 uAPI: `GPIO_GET_CHIPINFO_IOCTL`, `GPIO_V2_GET_LINEINFO_IOCTL`, `GPIO_V2_GET_LINE_IOCTL`, get values, edge event; self-test on gpio-sim | `sudo ./gpio_cdev_read [/dev/gpiochipN OFFSET]` |
| [user/i2c_smbus_rw.c](user/i2c_smbus_rw.c) | i2c-dev: `I2C_FUNCS`, `I2C_SLAVE`, raw `I2C_SMBUS` byte/word/I2C-block; self-test on i2c-stub; read (and `-w` write/verify/restore) on real buses | `sudo ./i2c_smbus_rw [BUS ADDR [REG] [-w]]` |
| [user/spidev_loopback.c](user/spidev_loopback.c) | spidev: mode/bits/speed ioctls, two-segment `SPI_IOC_MESSAGE`, rx == tx check | `sudo ./spidev_loopback [/dev/spidevB.C [HZ]]` |

User programs exit **0** on success, **1** if a check fails, **2** if the
device is not there (with a hint). `make test` does not run them.

**Build** (from the repo root):

```sh
make drivers                                                   # host: modules + user/ programs
make -C linux-device-drivers/07-gpio-i2c-spi-drivers/kernel W=1      # extra warnings
make -C linux-device-drivers/07-gpio-i2c-spi-drivers/kernel BUGS=1   # + esh_i2c_bug.ko
# board (paths: ../README.md#building)
make -C linux-device-drivers/07-gpio-i2c-spi-drivers/kernel \
     KDIR=$BB/KERNEL ARCH=arm64 CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-
aarch64-linux-gnu-gcc -O2 -static -o spidev_loopback linux-device-drivers/07-gpio-i2c-spi-drivers/user/spidev_loopback.c
make drivers-clean
```

**dtc** (overlays; `$BB` = the `sources/arm64-multiplatform` directory of the
beaglebone-ai64 workspace, as in `../README.md`):

```sh
cd linux-device-drivers/07-gpio-i2c-spi-drivers/kernel/dts
dtc -@ -I dts -O dtb -o esh-i2c2-devices.dtbo esh-i2c2-devices.dtso      # no #includes
cpp -nostdinc -undef -x assembler-with-cpp -I $BB/KERNEL/arch/arm64/boot/dts/ti \
    -I $BB/KERNEL/include esh-spi7-device.dtso | dtc -@ -I dts -O dtb -o esh-spi7-device.dtbo -
# optional: check they apply to the board's base DT
$BB/KERNEL/scripts/dtc/fdtoverlay -i $BB/KERNEL/arch/arm64/boot/dts/ti/k3-j721e-beagleboneai64.dtb \
    -o /tmp/merged.dtb esh-i2c2-devices.dtbo esh-spi7-device.dtbo
```

On the board: copy the `.dtbo`s to the overlay directory your
`/boot/firmware/extlinux/extlinux.conf` uses and list them on its
`fdtoverlays` line, then reboot (details in [06](../06-device-model-and-device-tree/)).

All modules are **build-tested only**: on the host (Ubuntu 22.04, kernel
6.8, zero warnings with and without `W=1`, checkpatch-clean apart from the
file-name note) and cross-built against the board's `6.18.x-arm64-k3`
PREEMPT_RT tree with its GCC 15. Both overlays compile with `dtc -@` and
apply to the board's base DTB with `fdtoverlay`. Loading the modules is
**your** step, in a VM or on the board ([../README.md](../README.md#safety)).
The output below is what the code prints; nobody has captured it from a
real run yet, so expect differences in the details.

#### VM test: I2C with i2c-stub

```sh
sudo modprobe i2c-dev
sudo modprobe i2c-stub chip_addr=0x48,0x50        # two fake chips, 256 registers each
N=$(i2cdetect -l | awk '/SMBus stub/ {sub("i2c-","",$1); print $1}'); echo bus $N
sudo i2cdetect -y $N                               # 48 and 50 appear

sudo ./build/linux-device-drivers/07-gpio-i2c-spi-drivers/user/i2c_smbus_rw
#   using i2c-stub on bus 9, address 0x50
#   /dev/i2c-9: functionality 0x...: byte-data word-data i2c-block
#   PASS byte data: wrote 0xa5 to reg 0x10 and read it back
#   PASS word data: 0x1234 -> reg 0x20 = 0x34, reg 0x21 = 0x12 (little endian)
#   PASS i2c block: 8 registers 0x30..0x37 read in one transfer

# EEPROM: preload two bytes, bind the driver, read through nvmem
sudo i2cset -y $N 0x50 0x00 0x45; sudo i2cset -y $N 0x50 0x01 0x53
sudo insmod linux-device-drivers/07-gpio-i2c-spi-drivers/kernel/esh_eeprom.ko
echo esh_eeprom 0x50 | sudo tee /sys/bus/i2c/devices/i2c-$N/new_device
sudo dmesg | tail -2
#   esh_eeprom 9-0050: 256 bytes at 0x50 on SMBus stub driver, SMBus I2C-block emulation, page 8
#   esh_eeprom 9-0050: PASS selftest: first 16 bytes identical via both paths: 45 53 00 00 ...
sudo hexdump -C /sys/bus/nvmem/devices/esh_eeprom0/nvmem | head -2
printf 'hello' | sudo dd of=/sys/bus/nvmem/devices/esh_eeprom0/nvmem bs=1 seek=6 conv=notrunc
sudo i2cget -y -f $N 0x50 0x06                     # 0x68 'h' (-f: the driver owns 0x50)

# Sensor: play the chip, then bind
sudo i2cset -y $N 0x48 0x0f 0xe5                   # WHO_AM_I
sudo i2cset -y $N 0x48 0x27 0x01                   # STATUS.DRDY
sudo i2cset -y $N 0x48 0x28 0x34; sudo i2cset -y $N 0x48 0x29 0x0c   # 0x0c34 = 3124
sudo insmod linux-device-drivers/07-gpio-i2c-spi-drivers/kernel/esh_sensor.ko rate=3
echo esh_sensor 0x48 | sudo tee /sys/bus/i2c/devices/i2c-$N/new_device
#   esh_sensor 9-0048: PASS selftest: WHO_AM_I 0xe5, CTRL 0x07 (EN=1 RATE=3) in cache and chip
cat /sys/bus/i2c/devices/$N-0048/temp              # 31.24
sudo i2cset -y $N 0x48 0x28 0x9c; sudo i2cset -y $N 0x48 0x29 0xff   # 0xff9c = -100
cat /sys/bus/i2c/devices/$N-0048/temp              # -1.00
echo 0 | sudo tee /sys/bus/i2c/devices/$N-0048/enable
sudo i2cget -y -f $N 0x48 0x20                     # 0x06: EN cleared by ONE bus write
cat /sys/bus/i2c/devices/$N-0048/temp              # error: No data available (ENODATA)
sudo cat /sys/kernel/debug/regmap/$N-0048/registers

# without the WHO_AM_I preload the probe refuses:
#   esh_sensor 9-0048: WHO_AM_I=0x00, expected 0xe5 (i2c-stub: i2cset -y 9 0x48 0x0f 0xe5)

echo 0x48 | sudo tee /sys/bus/i2c/devices/i2c-$N/delete_device
echo 0x50 | sudo tee /sys/bus/i2c/devices/i2c-$N/delete_device
sudo rmmod esh_sensor esh_eeprom i2c-stub
```

#### VM test: GPIO with gpio-sim

```sh
sudo modprobe gpio-sim
G=/sys/kernel/config/gpio-sim/esh
sudo mkdir $G $G/gpio-bank0
echo 8       | sudo tee $G/gpio-bank0/num_lines
echo esh-sim | sudo tee $G/gpio-bank0/label
echo 1       | sudo tee $G/live
CHIP=$(cat $G/gpio-bank0/chip_name); DEV=$(cat $G/dev_name); echo $CHIP $DEV
S=/sys/devices/platform/$DEV/$CHIP                  # sim_gpioN/{pull,value}

sudo ./build/linux-device-drivers/07-gpio-i2c-spi-drivers/user/gpio_cdev_read
#   gpiochip1: label "esh-sim", 8 lines
#     line   0: unnamed              -
#   ...
#   PASS gpiochip1 line 1: pull-down -> 0, pull-up -> 1
#   PASS gpiochip1 line 1: rising-edge event, timestamp 123456789 ns, seqno 1

echo pull-up | sudo tee $S/sim_gpio1/pull           # button released (active-low)
sudo insmod linux-device-drivers/07-gpio-i2c-spi-drivers/kernel/esh_gpio_demo.ko sim_chip=esh-sim
#   esh_gpio_demo: sim mode: chip "esh-sim", led line 0, button line 1 (active-low)
#   esh-gpio-demo esh-gpio-demo: PASS selftest: LED set 1 -> read 1, set 0 -> read 0 (active-high line)
#   esh-gpio-demo esh-gpio-demo: LED + button (irq 45, button released, can_sleep=1)
gpioinfo $CHIP                                      # lines 0/1: consumers esh-led / esh-button
echo pull-down | sudo tee $S/sim_gpio1/pull         # "press": falling edge -> IRQ
D=/sys/devices/platform/esh-gpio-demo
cat $D/presses $D/button $S/sim_gpio0/value         # 1 1 1   (LED followed the button)
echo pull-up | sudo tee $S/sim_gpio1/pull           # release
cat $D/presses $S/sim_gpio0/value                   # 1 0
sudo rmmod esh_gpio_demo
sudo insmod .../esh_gpio_demo.ko sim_chip=esh-sim led_active_low=1
echo 1 | sudo tee $D/led; cat $S/sim_gpio0/value    # 0: logical 1 = pad LOW
sudo rmmod esh_gpio_demo
echo 0 | sudo tee $G/live; sudo rmdir $G/gpio-bank0 $G
```

#### Board test: real parts on the header

```sh
# I2C: a 24C02 module (A0..A2 = GND -> 0x50) on P9.19 SCL / P9.20 SDA, 3V3, GND
sudo i2cdetect -y -r 2                              # 50 (or UU once the overlay binds it)
sudo ./i2c_smbus_rw 2 0x50 0x10                     # read only
sudo ./i2c_smbus_rw 2 0x50 0x10 -w                  # write/verify/restore one byte
# with esh-i2c2-devices.dtbo loaded:
sudo insmod esh_eeprom.ko                           # binds 2-0050 from DT
#   esh_eeprom 2-0050: 256 bytes at 0x50 on OMAP I2C adapter, raw i2c_transfer (repeated start), page 8
#   esh_eeprom 2-0050: PASS selftest: first 16 bytes identical via both paths: ...
# (the sensor at 0x48 is fictional: without a chip its probe fails with
#  "no answer at 0x48", which is the expected, correct behaviour)

# SPI loopback: esh-spi7-device.dtbo loaded, jumper P9.30 (MOSI) <-> P9.29 (MISO)
sudo insmod esh_spi_sensor.ko loopback=1
#   esh_spi_sensor spi7.0: on spi7 CS0, mode 0, 1000000 Hz
#   esh_spi_sensor spi7.0: PASS loopback: 32 bytes full duplex at 1000000 Hz, rx == tx
#   esh_spi_sensor spi7.0: INFO loopback: WHO_AM_I read on a looped bus = 0x00 (the dummy byte, not 0xe5)
# then the same wire from user space (bind spidev as in 3.5):
sudo ./spidev_loopback /dev/spidev7.0
#   /dev/spidev7.0: mode 0, 8 bits/word, max 1000000 Hz
#   PASS loopback: 32 + 7 bytes in one message, rx == tx
# pull the jumper:  FAIL rx != tx ... MISO is constant: no MOSI->MISO jumper ...
```

The SPI bus number (`spi7` above) is assigned at boot; the board DTS has
no `spi` aliases. Check `ls /sys/bus/spi/devices/`.

---

## 9. Exercises

**Easy**
1. Run `gpioinfo` on the board and find P8.18 and P9.19. Why does P9.19
   appear twice (`P9_19a`, `P9_19b`)? Which one is muxed to I2C?
2. With i2c-stub, make `esh_sensor` report −40.00 °C. Which two bytes?
3. Load `esh_eeprom.ko page_size=3`. What happens and why is a power of two required?

**Medium**
4. Add a `write_protect` GPIO to `esh_eeprom` (`devm_gpiod_get_optional(dev,
   "wp", GPIOD_OUT_HIGH)`), drop it around each write. Test with gpio-sim
   (lookup table) or on the board.
5. Turn `esh_sensor` into a **hwmon** driver (`devm_hwmon_device_register_with_info`,
   `hwmon_temp_input` in millidegrees) and read it with `sensors`.
6. Count bus transactions: enable the `regmap` and `i2c` tracepoints
   (`/sys/kernel/tracing/events/regmap/`, `.../i2c/`) and show that
   toggling `enable` twice costs exactly two bus writes.
7. Write an overlay that disables `main_i2c6` and enables SPI6 instead on
   P9.17/P9.18/P9.21/P9.22 (take the pads from the vendor
   `-spi-mcspi6-cs0.dtso`). Run the loopback test there.

**Hard**
8. Debounce the button in `esh_gpio_demo` in software: on each edge
   (re)arm a 20 ms `hrtimer` or delayed work, sample the line when it
   fires. Prove with `gpio-sim` (fast pull toggles) that bounces are
   filtered.
9. Use `spi_async()` in `esh_spi_sensor` from a periodic hrtimer to read
   DATA at 1 kHz without sleeping, and push samples into a kfifo read
   by a char device (lessons 01 and 03). Which context does the completion
   run in?
10. Write a `gpio_chip` provider: a module that exposes 8 GPIO lines
    backed by the CTRL register of the fictional sensor over regmap
    (`devm_gpiochip_add_data`, `.can_sleep = true`). Check it with
    `gpioinfo` and point `esh_gpio_demo` at it.

---

## 10. Remember forever

```
Bus core in the middle: controller driver (TI, per SoC) <-> core <-> client driver (you).
GPIO: devm_gpiod_get(dev,"led",GPIOD_OUT_LOW) <- DT led-gpios; values LOGICAL; active-low = DT flag.
      _cansleep in process ctx; gpiod_to_irq + threaded IRQ + IRQF_ONESHOT. Legacy gpio numbers/sysfs: deprecated.
I2C:  i2c_driver{of_match_table, id_table, probe(client)}; check_functionality first;
      reg read = ONE i2c_transfer with 2 msgs (Sr); smbus_* return int (<0 = errno). Transfers SLEEP.
      instantiate: DT child reg=<addr> | echo name addr > .../new_device. User: /dev/i2c-N + I2C_SLAVE.
regmap: config once (reg/val bits, volatile, cache) -> regmap_read/update_bits/bulk_read, any bus.
SPI:  spi_driver + spi_device_id; message = transfers under one CS; full duplex; kmalloc buffers (DMA).
      modes CPOL/CPHA 0..3; spidev via driver_override, never compatible="spidev". Loopback = MOSI->MISO.
AI-64: header pins have a/b twin pads: mux one, park the other; i2c-2 = P9.19/20; SPI7 = P9.28-31.
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Draw the controller/core/client stack for I2C and name the TDA4VM
   controller driver. What does a client driver never need to know?
2. How does `devm_gpiod_get(dev, "reset", ...)` find the line in DT? In a
   system without DT?
3. A button is wired to GND with a pull-up. Which DT flag, and what does
   `gpiod_get_value()` return when it is pressed?
4. When must you use `gpiod_set_value_cansleep()`? What happens with the
   non-cansleep version on an I2C expander?
5. Draw an I2C register read on the wire. Why must the two messages go
   into a single `i2c_transfer()` call?
6. Why does `esh_eeprom` check `i2c_check_functionality()`, and what does
   it do on i2c-stub versus the TDA4VM controller?
7. Name three things regmap does for you. What makes a register
   "volatile", and what goes wrong if you forget to mark one?
8. Explain SPI modes 0–3. Why does reading n bytes over SPI cost n
   transmitted bytes?
9. Why must `spi_transfer` buffers not live on the stack, and why is
   `spi_write_then_read()` the exception?
10. Why does the kernel refuse `compatible = "spidev"`, and what are the
    two correct ways to get `/dev/spidevB.C`?
11. What is pinmuxing, which driver does it on the TDA4VM, and when is the
    `default` state applied? What is special about the AI-64 header pins?
12. You get `EBUSY` from `ioctl(I2C_SLAVE)`. What does it mean and what
    should you do?
