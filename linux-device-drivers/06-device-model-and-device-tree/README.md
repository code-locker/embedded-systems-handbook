# 06 — The Device Model and the Device Tree

> How does the kernel know there is a UART at `0x02800000` on the
> BeagleBone AI-64, which driver handles it, and in what order things must
> be set up and torn down? This lesson covers the **driver model** (bus /
> device / driver, matching, probe/remove, deferred probe, `devm_*`), the
> **platform bus** (resources, MMIO with `readl`/`writel`), and the
> **Device Tree** that describes an ARM board: DTS syntax, the DTB that
> U-Boot hands to Linux, `of_match_table` and module autoloading, reading
> properties in a firmware-agnostic way, bindings, and **overlays** applied
> by U-Boot on the BeagleBone. Two modules prove it: one builds a device +
> driver pair from software nodes on any PC, one is a real DT driver (with a
> consumer that *defers* its probe) for an overlay you load on the board.

---

## 1. Why this exists

A PC discovers its hardware: PCI and USB devices announce a vendor/device
ID, ACPI tables describe the rest. A system-on-chip cannot: the TDA4VM's
UARTs, I2C controllers, GPIO blocks and timers sit at fixed addresses on
internal buses that nobody can enumerate. Something must tell the kernel
"there is a `ti,j721e-uart` at `0x02800000`, its interrupt is GIC SPI 192,
its clock is 48 MHz, it is wired to these pins".

Before ~2011 that "something" was C code: one **board file** per board in
`arch/arm/mach-*/`, registering `platform_device`s by hand. Thousands of
them, a kernel per board. The **Device Tree** replaced them with data: a
text description of the board (`.dts`) compiled to a binary blob (`.dtb`)
that the bootloader passes to one generic kernel. The same arm64 kernel
image boots the BeagleBone AI-64, a Raspberry Pi and a server.

On top of that sits the **device model**, the kernel's bookkeeping for
every device: which bus it is on, which driver owns it, which devices it
depends on, in what order to suspend or remove them, and the `/sys` tree
that shows it all. Every driver you will ever write plugs into it — the
GPIO button of [04](../04-interrupts-timers-and-deferred-work/) did, as do
the I2C/SPI drivers of [07](../07-gpio-i2c-spi-drivers/).

---

## 2. Mental model

**A staffing agency.** *Buses* are agencies (platform, I2C, SPI, PCI,
USB). *Devices* are job openings posted with the agency, each with a job
description (DT node: `compatible`, `reg`, `interrupts`...). *Drivers* are
workers registered with the agency, each listing the job titles they can
do (`of_match_table`). Whenever a new opening or a new worker appears, the
agency tries to **match** them; on a match the worker is sent to
**probe** the job. If a tool the worker needs is not there yet (a clock, a
GPIO, a regulator), the worker says "call me back later"
(`-EPROBE_DEFER`). When the job ends (**remove**), everything the worker
borrowed with `devm_*` is returned automatically, last borrowed first.

```
                         struct bus_type  (platform_bus_type, i2c_bus_type, ...)
                        /              \
        devices list   /                \   drivers list
   +----------------------+          +---------------------------+
   | struct device        |          | struct device_driver      |
   |  "2800000.serial"    |  match() |  "omap8250"               |
   |  of_node ------------+--------->|  of_match_table:          |
   |   compatible =       |  compare |   "ti,am654-uart" ...     |
   |   "ti,j721e-uart",   |  strings |                           |
   |   "ti,am654-uart"    |          |  probe() / remove()       |
   |  resources: MEM, IRQ |          +---------------------------+
   |  driver  ------------+--> (set when probe() returned 0)
   |  devres list (devm)  |
   +----------------------+
        |  shows up as /sys/devices/platform/bus@100000/2800000.serial
        |  linked from /sys/bus/platform/devices/ and .../drivers/omap8250/
```

The sentence to remember: **the DT describes, the bus matches, the driver
probes — and everything acquired with devm in probe is released, in
reverse order, after remove.**

---

## 3. How it works

### 3.1 Bus, device, driver — matching and binding

Three structures, all in `<linux/device.h>`:

- **`struct device`** — embedded in every device (`platform_device.dev`,
  `i2c_client.dev`, ...). Holds the parent, the bus, the bound driver,
  the firmware node (`fwnode`, `of_node`), driver data
  (`dev_get_drvdata()`), the devres list, power-management state, and the
  sysfs directory.
- **`struct device_driver`** — embedded in `platform_driver.driver`,
  `i2c_driver.driver`... Holds the name, the match tables, `dev_groups`
  (sysfs attributes created on bind), PM ops.
- **`struct bus_type`** — owns both lists and the `match()` callback.

```
 device_add() or driver_register()
        |
        v
 bus->match(dev, drv) for every candidate pair          platform_match() order:
        |  true                                            1. driver_override (sysfs)
        v                                                  2. of_driver_match_device()  DT "compatible"
 really_probe():                                           3. ACPI match
   dev->driver = drv; pinctrl "default" state applied      4. id_table (platform_device_id names)
   drv->probe(dev)                                         5. device name == driver name
        |  0 -> bound: sysfs "driver" link, dev_groups created, uevent BIND
        |  -EPROBE_DEFER -> put on the deferred list, retried later
        |  other error   -> devres released, device stays unbound
        v
 ... later: unbind (rmmod, device_del, echo > unbind, supplier going away)
   drv->remove(dev); devres_release_all(dev)    <- reverse order of acquisition
```

You can drive it by hand:

```sh
ls /sys/bus/platform/drivers/omap8250/                 # devices bound to this driver
echo 2800000.serial | sudo tee /sys/bus/platform/drivers/omap8250/unbind   # careful: console!
echo 2800000.serial | sudo tee /sys/bus/platform/drivers/omap8250/bind
echo mydrv | sudo tee /sys/bus/platform/devices/X/driver_override          # force a match
```

`platdev_host_demo` does the same from code: `device_release_driver()`
then `device_attach()`, and checks that probe ran twice.

### 3.2 Probe, remove and deferred probe

`probe()` gets the device ready: read the description, claim resources,
map registers, check the hardware ID, register with the subsystem
(input, tty, net...). `remove()` stops the hardware; with devm, freeing is
automatic. Since **6.11** `platform_driver.remove` returns `void`
(6.3–6.12 had the transitional `.remove_new`); the modules here use a
`LINUX_VERSION_CODE` guard so they build on both the 6.8 host and the 6.18
board.

**Deferred probe.** Drivers bind in whatever order modules load and devices
appear. If probe needs something that does not exist yet — a clock, a
regulator, a GPIO controller, a PHY, *another device's driver* — it returns
**`-EPROBE_DEFER`**. The core parks the device on a list and retries all
parked devices each time any other device binds successfully.

```c
gpiod = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
if (IS_ERR(gpiod))
        return dev_err_probe(dev, PTR_ERR(gpiod), "reset GPIO\n");
```

`dev_err_probe()` prints the error *unless* it is `-EPROBE_DEFER` (no log
spam on every retry), records the reason, and returns the error code.
Inspect what is waiting and why: `cat /sys/kernel/debug/devices_deferred`.
If something stays there forever, its supplier never came (driver not
built, overlay not applied, typo in a phandle).

**fw_devlink** (default on) goes further: it reads standard DT properties
(`clocks`, `gpios`, `*-supply`, `pinctrl-0`, `interrupts`, `phys`...),
creates **device links** consumer → supplier from the DT alone, and does
not even try to probe a consumer before its suppliers are bound. A device
link also orders suspend/resume and forces consumers to unbind before
their supplier. `esh_demo_driver`'s consumer uses a *custom* phandle
property (`esh,provider`) that fw_devlink does not know, so it shows the
classic path: look up the supplier, defer while it is not bound, then add
the link itself with `device_link_add(dev, sup, DL_FLAG_AUTOREMOVE_CONSUMER)`.
The links appear as `/sys/class/devlink/` and `supplier:*` / `consumer:*`
symlinks in the devices' sysfs directories.

### 3.3 `devm_*` — managed resources

Every `devm_` call allocates a small **devres** record on the device's list
together with the resource. When the device unbinds (or probe fails), the
core walks the list **backwards** and releases each one.

```
probe:   devm_kzalloc(p)      -> list: [p]
         devm_clk_get(clk)    -> list: [p, clk]
         devm_ioremap(regs)   -> list: [p, clk, regs]
         devm_request_irq(irq)-> list: [p, clk, regs, irq]
unbind:  remove()  then free irq -> unmap regs -> put clk -> kfree p     (reverse)
```

Why it matters: error paths in probe become `return dev_err_probe(...)`
instead of a goto ladder, and teardown cannot forget anything. Where it
bites:

- **Order.** The IRQ is freed *after* `remove()` returns. If `remove()`
  frees something the handler uses, the handler can run on freed memory.
  Either do not free it in `remove()` (make it devm too) or register a
  `devm_add_action_or_reset(dev, fn, data)` *before* `devm_request_irq()`
  so it runs *after* the IRQ is gone ([04 §3.7](../04-interrupts-timers-and-deferred-work/)).
- **Lifetime.** devm memory lives as long as the *binding*, not as long as
  open files: a char device still open after unbind must not point into it.
- **Mixing.** Do not `kfree()` a `devm_kzalloc()` pointer, and do not mix
  devm and manual release of the same resource class in a way that changes
  the order.

`platdev_host_demo` registers three no-op actions A, B, C and checks they
run as C, B, A at unbind.

### 3.4 The platform bus, resources and MMIO

A **platform device** is a device on a bus that cannot be enumerated —
exactly the SoC peripherals. On a DT system `of_platform_populate()`
creates one for every node directly under `/` and under every
`simple-bus` node whose `status` is `"okay"`; its name is
`<unit-address>.<node-name>` (`2800000.serial`) or just the node name.

```c
struct platform_driver my_driver = {
        .probe  = my_probe,
        .remove = my_remove,                       /* void since 6.11 */
        .driver = { .name = "my", .of_match_table = my_of_match, .dev_groups = my_groups },
        .id_table = my_ids,                        /* optional: match by name */
};
module_platform_driver(my_driver);                 /* init = register, exit = unregister */
```

**Resources** are the device's MMIO ranges and IRQs, filled in from `reg`
and `interrupts`:

```c
struct resource *r = platform_get_resource(pdev, IORESOURCE_MEM, 0);   /* start, end, flags */
void __iomem *base = devm_platform_ioremap_resource(pdev, 0);          /* request + ioremap */
int irq = platform_get_irq(pdev, 0);              /* virq; logs an error if missing */
int irq2 = platform_get_irq_optional(pdev, 1);    /* -ENXIO if missing, no log */
```

`devm_platform_ioremap_resource()` = `devm_request_mem_region()` (exclusive
ownership — it shows in `/proc/iomem` and a second driver gets `-EBUSY`)
+ `devm_ioremap()` (a *device* mapping: uncached, no speculation, no write
merging) — both undone at unbind.

**Registers.** Never dereference an `__iomem` pointer directly; use the
accessors (`<linux/io.h>`):

| Accessor | Ordering | Use |
|---|---|---|
| `readl(a)` / `writel(v, a)` (`b`/`w`/`l`/`q` = 8/16/32/64 bit) | ordered against **DMA** and other MMIO: `writel` = `wmb()` + store; `readl` = load + `rmb()` | default choice |
| `readl_relaxed()` / `writel_relaxed()` | ordered only against other accesses to the *same device*; no barrier | hot paths with no DMA dependency (FIFO loops) |
| `ioread32()` / `iowrite32()` | like `readl`, also work on port I/O | PCI drivers |
| `readl_poll_timeout(addr, val, cond, sleep_us, timeout_us)` | — | wait for a status bit |

The classic bug the non-relaxed versions prevent: fill a DMA descriptor in
RAM, then `writel_relaxed()` the "go" bit — on arm64 the device may see the
doorbell before the descriptor reaches memory. `writel()`'s barrier
forbids that. MMIO is little-endian by convention for these accessors
(`ioread32be()` for big-endian blocks).

**Register maps** are bit fields; write them with `<linux/bitfield.h>`:

```c
#define CTRL_MODE  GENMASK(7, 4)
ctrl = readl(base + CTRL);
ctrl &= ~CTRL_MODE;
ctrl |= FIELD_PREP(CTRL_MODE, 3);       /* compile-time checked against the mask */
writel(ctrl, base + CTRL);
mode = FIELD_GET(CTRL_MODE, readl(base + CTRL));
```

The bit-manipulation behind it (masks, read-modify-write, why RMW races)
is [dsa/18-bit-manipulation §3.4](../../dsa/18-bit-manipulation/) and its
`03_register_bitfields.c`. Larger drivers use **regmap**
(`devm_regmap_init_mmio()`), which adds caching, locking and debugfs dumps
on top — and works the same for I2C/SPI register maps ([07](../07-gpio-i2c-spi-drivers/)).

On the PC there is no device to map, so `platdev_host_demo` hangs its MEM
resource under a private fake bus (not `/proc/iomem`) and, because its node
says `esh,simulated-regs`, uses 256 bytes of RAM as "registers". The driver
code around it is exactly what talks to real hardware.

### 3.5 Device Tree: why, and the syntax

**Why DT on ARM, ACPI on x86.** A PC firmware (UEFI) ships ACPI tables
written by the board vendor, with bytecode methods for power management.
ARM SoC boards traditionally boot through U-Boot and describe the hardware
with a DT — pure data, no code; the kernel's drivers do the work. (ARM
servers use ACPI too; the kernel's `fwnode`/`device_property` layer hides
the difference — §3.7.)

**Syntax** — a tree of nodes with properties:

```dts
/dts-v1/;
/ {                                         /* root node */
        #address-cells = <2>;               /* children's reg: 2 cells of address... */
        #size-cells = <2>;                  /* ...and 2 cells of size (64-bit each)  */
        compatible = "beagle,j721e-beagleboneai64", "ti,j721e";   /* most specific first */
        model = "BeagleBoard.org BeagleBone AI-64";

        aliases { serial2 = &main_uart0; };  /* stable names: ttyS2, i2c1, mmc0 */
        chosen { stdout-path = "serial2:115200n8"; };   /* console; bootargs, initrd */

        label: node-name@unit-address {      /* unit address = first reg address */
                compatible = "vendor,device";        /* string list: driver match */
                reg = <0x00 0x02800000 0x00 0x100>;  /* <addr-hi addr-lo size-hi size-lo> */
                interrupts = <GIC_SPI 192 IRQ_TYPE_LEVEL_HIGH>;  /* format set by the parent's #interrupt-cells */
                clocks = <&k3_clks 146 0>;           /* phandle + specifier cells */
                status = "okay";                     /* "disabled": no device is created */
                some-flag;                           /* boolean: present = true */
                values = <1 2 3>;                    /* u32 array ("cells") */
                bytes = [de ad be ef];
        };
};
&label { status = "okay"; };                 /* later: modify a node by its label */
```

| Item | Meaning |
|---|---|
| `compatible` | list of strings, most specific first; the kernel tries each against every driver's `of_match_table` |
| `reg` | address/size pairs, counted in the **parent's** `#address-cells` / `#size-cells` (I2C: `reg = <0x50>` = slave address, `#size-cells = <0>`) |
| `interrupts`, `interrupt-parent` | IRQ specifier, decoded by the interrupt controller named by `interrupt-parent` (inherited from ancestors) |
| phandle `<&label ...>` | a reference to another node, e.g. `gpios = <&main_gpio0 96 GPIO_ACTIVE_HIGH>` |
| `status` | `"okay"` or `"disabled"` (SoC `.dtsi` files disable everything; the board `.dts` enables what is wired) |
| `pinctrl-names`, `pinctrl-0` | pin-mux states, applied by the driver core before probe |
| `ranges` | address translation from a bus to its parent (empty `ranges;` = 1:1) |
| `aliases`, `chosen` | special nodes: stable numbering; boot parameters / console |
| `#include`, `GIC_SPI`, `GPIO_ACTIVE_LOW` | the DTS is run through the **C preprocessor** first; macros come from `include/dt-bindings/` |

**Files.** SoC `.dtsi` (`k3-j721e.dtsi`, `-main.dtsi`, `-mcu-wakeup.dtsi`)
→ board `.dts` (`k3-j721e-beagleboneai64.dts`) → `dtc` → `.dtb` (flattened
binary: header, structure block, strings block). The kernel builds them:
`make dtbs` → `arch/arm64/boot/dts/ti/*.dtb`.

**Boot.** U-Boot loads the DTB (and kernel) into RAM, applies overlays,
fixes up a few properties (memory size, MAC addresses, `chosen/bootargs`),
and passes its address in register `x0` at `booti ${kernel_addr_r} -
${fdt_addr_r}`. The beaglebone-ai64 workspace documents the load addresses
(`fdt_addr_r = 0x88000000`, `fdtoverlay_addr_r = 0x89000000`) in its
`docs/build/TFTP-BOOT.md`. Early in boot the kernel "unflattens" the DTB
into `struct device_node`s and later `of_platform_populate()` creates the
devices.

### 3.6 Walking the real BeagleBone AI-64 device tree

**A UART: base description in the SoC file, enabled by the board.**

```dts
/* k3-j721e-main.dtsi — the SoC: every UART exists, none is enabled */
main_uart0: serial@2800000 {
        compatible = "ti,j721e-uart", "ti,am654-uart";  /* 8250_omap.c matches "ti,am654-uart" */
        reg = <0x00 0x02800000 0x00 0x100>;    /* 256 bytes at 0x0280_0000 (2+2 cells) */
        interrupts = <GIC_SPI 192 IRQ_TYPE_LEVEL_HIGH>;  /* straight to the GIC: INTID 224 */
        clock-frequency = <48000000>;          /* the "fclk" baud-rate generator input */
        power-domains = <&k3_pds 146 TI_SCI_PD_EXCLUSIVE>;  /* firmware powers it on */
        clocks = <&k3_clks 146 0>;
        clock-names = "fclk";
        status = "disabled";
};

/* k3-j721e-beagleboneai64.dts — the board: this one is the debug console */
&main_uart0 {
        status = "okay";
        pinctrl-names = "default";
        pinctrl-0 = <&main_uart0_pins_default>;     /* pads 0x1e8/0x1ec -> UART0 RX/TX, mode 0 */
        power-domains = <&k3_pds 146 TI_SCI_PD_SHARED>;   /* "Shared with ATF on this platform" */
};
/ { aliases { serial2 = &main_uart0; };  chosen { stdout-path = "serial2:115200n8"; }; };
```

The result is `/sys/devices/platform/bus@100000/2800000.serial`, bound to
`omap8250`, appearing as `/dev/ttyS2` (because of the alias) — the serial
console you watch the board boot on.

**An I2C bus with a device on it.**

```dts
&wkup_i2c0 {                              /* i2c@42120000 in k3-j721e-mcu-wakeup.dtsi */
        status = "okay";
        pinctrl-names = "default";
        pinctrl-0 = <&wkup_i2c0_pins_default>;    /* pull-ups on SCL/SDA pads */
        clock-frequency = <400000>;               /* 400 kHz fast mode */

        eeprom@50 {                       /* a CHILD = a device on this bus */
                compatible = "atmel,24c04";       /* drivers/misc/eeprom/at24.c */
                reg = <0x50>;                     /* I2C address (parent: #size-cells = <0>) */
                pinctrl-names = "default";
                pinctrl-0 = <&eeprom_wp_pins_default>;   /* the write-protect pin */
        };
};
```

The I2C controller is a platform device; its driver (`i2c-omap`) registers
an I2C *adapter*, which then creates an `i2c_client` for each child node —
a device on another bus (`/sys/bus/i2c/devices/0-0050`). This board-ID
EEPROM is what U-Boot reads to know which board it runs on. (`i2c0` in
`aliases` makes the adapter number stable.)

**GPIO LEDs: a "virtual" device that consumes GPIOs.**

```dts
leds {                                    /* no reg: not on any bus, a root-level node */
        compatible = "gpio-leds";         /* drivers/leds/leds-gpio.c */
        pinctrl-names = "default";
        pinctrl-0 = <&led_pins_default>;  /* pads -> GPIO mode (mux 7) */

        led-0 {
                gpios = <&main_gpio0 96 GPIO_ACTIVE_HIGH>;   /* phandle + line + flags */
                function = LED_FUNCTION_HEARTBEAT;
                linux,default-trigger = "heartbeat";         /* a timer_list blinks it */
        };
};
/* ...and k3-j721e-beagleboneai64-pinmux.dtsi adds  label = "beaglebone:green:usr0"; */
```

One platform device (`leds`), children parsed by the driver
(`device_for_each_child_node()`), → `/sys/class/leds/beaglebone:green:usr0/`.
`gpios = <&main_gpio0 ...>` is also what fw_devlink turns into a device
link: `leds` is not probed before `gpio@600000` is.

### 3.7 Matching and reading properties in a driver

```c
static const struct of_device_id my_of_match[] = {
        { .compatible = "esh,demo-device", .data = &variant_a },
        { }
};
MODULE_DEVICE_TABLE(of, my_of_match);
```

`MODULE_DEVICE_TABLE` writes aliases into the `.ko`
(`modinfo esh_demo_driver.ko` → `alias: of:N*T*Cesh,demo-device*`).
`depmod` collects them into `modules.alias`; when the kernel creates the
device it emits a uevent with `MODALIAS=of:Nesh-demoT...Cesh,demo-device`,
and udev runs `modprobe` on it — the module loads itself. Get the `.data`
of the entry that matched with `device_get_match_data(dev)`.

**Reading properties — prefer the firmware-agnostic API** (`<linux/property.h>`):

| `device_property_*` / `fwnode_property_*` (DT, ACPI, software nodes) | `of_property_*` (DT only) |
|---|---|
| `device_property_read_u32(dev, "x", &v)` | `of_property_read_u32(np, "x", &v)` |
| `device_property_read_u32_array(dev, "x", arr, n)` | `of_property_read_u32_array()` |
| `device_property_count_u32(dev, "x")` | `of_property_count_u32_elems()` |
| `device_property_read_string(dev, "x", &s)` | `of_property_read_string()` |
| `device_property_read_bool(dev, "x")` / `device_property_present()` | `of_property_read_bool()` |
| `device_for_each_child_node(dev, child)` | `for_each_child_of_node(np, child)` |
| `fwnode_find_reference(fwnode, "x", 0)` | `of_parse_phandle(np, "x", 0)` |

All return `-EINVAL` if the property is missing and `-EOVERFLOW`/`-ENODATA`
on a size mismatch, and leave the output untouched on error — so set the
default first, then read. Iterators hold a reference on `child`: leave a
loop early only after `fwnode_handle_put(child)` (or use the `_scoped`
variants of 6.12+).

**Software nodes** (`PROPERTY_ENTRY_U32()`, `PROPERTY_ENTRY_STRING()`,
`PROPERTY_ENTRY_U32_ARRAY()`, `PROPERTY_ENTRY_BOOL()`,
`PROPERTY_ENTRY_REF()`, `struct software_node` with `.parent` for
children) let C code attach a DT-like description to a device:
`device_add_software_node()` or `platform_device_info.fwnode =
software_node_fwnode()`. Platform code on x86 uses them for devices ACPI
describes badly; here they let the *same* driver code run on the PC.

**Bindings.** Every `compatible` must be documented in
`Documentation/devicetree/bindings/` as a YAML schema (json-schema): which
properties are required, their types and ranges, an example. `make
dt_binding_check` validates the schemas; `make dtbs_check` validates every
DTS against them. Vendor prefixes (`ti,`, `atmel,`, `esh,`) are registered
in `vendor-prefixes.yaml`; `esh,` here is a made-up prefix for this
handbook — a binding sketch for `esh,demo-device` is exercise 7.

### 3.8 Overlays — changing the DT without rebuilding it

A **DT overlay** (`.dtso` source → `.dtbo` binary) is a patch applied to
the base DTB: add nodes, change properties, set `status = "okay"`. Capes,
HATs and "which header pins are what" on Beagle boards are overlays. Two
equivalent syntaxes:

```dts
/dts-v1/;
/plugin/;                                   /* this is an overlay */

&{/} {                                      /* new syntax: target by path ...      */
        esh-demo { compatible = "esh,demo-device"; ... };
};
&main_i2c2 { status = "okay"; };            /* ... or by a label of the base DT     */

/* old syntax, what dtc generates from the above: */
/ { fragment@0 { target-path = "/";   __overlay__ { esh-demo { ... }; }; };
    fragment@1 { target = <&main_i2c2>; __overlay__ { status = "okay"; }; }; };
```

Compile with **`dtc -@`**: it emits `__symbols__` (the overlay's own labels)
and `__fixups__` (references to base-DT labels, resolved at apply time). The
base DTB must also be built with `-@` — the kernel does that for TI boards
(`DTC_FLAGS := -@` in `arch/arm64/boot/dts/ti/Makefile`). The kernel tree
ships many BeagleBone AI-64 overlays next to the DTS
(`k3-j721e-beagleboneai64-*.dtso`) — read them for real pinmux examples.
Gotcha learned from them: an overlay that sets `pinctrl-0` on an existing
node (`&main_gpio0 { pinctrl-0 = <&my_pins>; }`) **replaces** the base
DT's list for that node; put `pinctrl-0` on your *own* node instead, as
[04's button overlay](../04-interrupts-timers-and-deferred-work/kernel/gpio-button-overlay.dtso) does.

**Applying on the BeagleBone AI-64** happens in U-Boot, before Linux
starts (mainline Linux has no stable user interface for runtime overlays).
BeagleBoard.org Debian boots through extlinux; the beaglebone-ai64
workspace's `deploy-and-test` and `kernel-dev` skills describe the board's
boot files and kernel/DTB install:

```sh
make -C linux-device-drivers/06-device-model-and-device-tree/kernel dtbo   # -> esh-demo-overlay.dtbo
# optional offline check against the board's base DTB:
BB=~/beaglebone-ai64/sources/arm64-multiplatform          # adjust the path
fdtoverlay -i $BB/KERNEL/arch/arm64/boot/dts/ti/k3-j721e-beagleboneai64.dtb \
           -o /tmp/test.dtb linux-device-drivers/06-device-model-and-device-tree/kernel/esh-demo-overlay.dtbo
scp kernel/esh-demo-overlay.dtbo kernel/esh_demo_driver.ko debian@<board-ip>:
# on the board:
sudo cp esh-demo-overlay.dtbo /boot/firmware/overlays/
sudoedit /boot/firmware/extlinux/extlinux.conf     # in your boot label:
#     fdtoverlays /overlays/esh-demo-overlay.dtbo   (space-separated list for several)
sudo reboot
```

By hand at the U-Boot prompt:

```
=> load mmc 1:1 ${fdt_addr_r} k3-j721e-beagleboneai64.dtb
=> fdt addr ${fdt_addr_r}
=> fdt resize 8192                       # room for the new nodes
=> load mmc 1:1 ${fdtoverlay_addr_r} overlays/esh-demo-overlay.dtbo
=> fdt apply ${fdtoverlay_addr_r}
=> fdt print /esh-demo                   # check before booting
```

### 3.9 Inspecting the live device tree

```sh
ls /sys/firmware/devicetree/base/             # the unflattened tree (= /proc/device-tree)
cat /proc/device-tree/model; echo
cat /proc/device-tree/esh-demo/label          # strings are NUL-terminated
xxd /proc/device-tree/esh-demo/esh,thresholds # cells are big-endian u32
dtc -I fs -O dts /sys/firmware/devicetree/base > live.dts   # decompile what the kernel sees
ls -l /sys/bus/platform/devices/esh-demo/of_node            # device -> its DT node
dtc -I dtb -O dts k3-j721e-beagleboneai64.dtb | less        # decompile a DTB file
```

`dtc -I fs` is the ultimate "did my overlay apply?" check: if the node is
not in `live.dts`, no driver will ever see it.

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| matching a new device | O(drivers on the bus × table entries) string compares | `bus_for_each_drv()` + `of_match_node()`; done once per device |
| matching a new driver | O(devices on the bus) | `bus_for_each_dev()` |
| deferred probe retry | O(deferred devices) per successful bind | the whole list is retried — many deferrals slow boot, which is why fw_devlink exists |
| devres release | O(resources), in reverse | walk of the devres list |
| `device_property_read_*` | O(properties of the node) | linear search by name in the node's property list (DT and swnode) |
| `of_find_node_by_path()` / phandle lookup | O(path depth) / O(1) with the phandle cache | |
| `readl()` vs `readl_relaxed()` | one load (+ a barrier: `dmb` on arm64) | the barrier costs tens of cycles; only matters in tight loops |
| DT size | the BBAI-64 DTB is ~100 KiB | parsed once at boot |

---

## 5. C vs C++ — and kernel vs user space

The device model is C written in an object-oriented style: `struct device`
is the **base class** embedded in every concrete device, `container_of()`
is the **downcast** (`to_platform_device(dev)`), function-pointer tables
(`struct platform_driver`, `struct bus_type`) are **vtables**, kobject
refcounts are **shared ownership** (`get_device`/`put_device` ≈
`std::shared_ptr` copies), and devres is **RAII** done by hand: a stack of
destructors run in reverse order, like C++ destroys members and locals.

User space sees the model through sysfs and uevents:

| Kernel | User space |
|---|---|
| `struct device` | a directory under `/sys/devices/` |
| bound driver | the `driver` symlink in that directory |
| `dev_groups` attributes | files in that directory (`cat`, `echo`) |
| uevent `ADD`/`BIND`, `MODALIAS` | `udevadm monitor --kernel --udev`, udev rules, `modprobe` |
| DT node | `/proc/device-tree/...`, `of_node` symlink |
| `MODULE_DEVICE_TABLE` | `modinfo`, `/lib/modules/$(uname -r)/modules.alias` |

`user/devmodel_sysfs.c` walks all of these for the two modules.
libudev/sd-device are the C APIs over the same files. Sysfs attributes in
depth are lesson [05-sysfs-procfs-debugfs](../05-sysfs-procfs-debugfs/).

---

## 6. Where it is used in the real world

- **Every SoC driver** in `drivers/`: `8250_omap.c` (the board's UARTs),
  `i2c-omap.c`, `gpio-davinci.c`, `leds-gpio.c`, `gpio_keys.c`, `at24.c` —
  each an `of_match_table` + probe with devm.
- **`drivers/of/`**: `fdt.c` (unflatten at boot), `platform.c`
  (`of_platform_populate`), `property.c` (fw_devlink), `overlay.c`.
- **`drivers/base/`**: `core.c` (devices, device links), `dd.c` (probe,
  deferred probe), `devres.c`, `swnode.c` (software nodes),
  `platform.c` (platform bus).
- **U-Boot** uses the *same* DTS files (its own copy, plus
  `-u-boot.dtsi` additions) for its own driver model — the beaglebone-ai64
  workspace builds it.
- **Zephyr RTOS** describes boards with Devicetree too, but resolves it
  at *build time* into C macros.
- **x86 platform code** (`drivers/platform/x86/`) attaches software nodes
  to ACPI-described devices to supply missing properties — the trick
  `platdev_host_demo` uses.
- **BeagleBoard.org overlays** for capes (`BBORG_MOTOR`, PWM/SPI/I2C pin
  setups) in the board kernel tree.

---

## 7. Common mistakes and pitfalls

1. **`compatible` typo** in DTS or driver: no error at all, the driver just
   never probes. Check `/sys/bus/platform/devices/<dev>/of_node/compatible`
   against `modinfo`'s aliases.
2. **Forgetting `status = "okay"`** when enabling a SoC block (the `.dtsi`
   says `"disabled"`): no device, no probe, no message.
3. **Missing `MODULE_DEVICE_TABLE(of, ...)`**: works with `insmod`, never
   autoloads.
4. **Logging errors on `-EPROBE_DEFER`** (or treating it as fatal): use
   `dev_err_probe()`. And never return `-EPROBE_DEFER` for something that
   will *never* appear — the device sits in `devices_deferred` forever.
5. **devm teardown order vs IRQs/work** (§3.3): freeing in `remove()` what
   a still-registered devm IRQ handler uses.
6. **Using `of_property_*` in new drivers**: DT-only; `device_property_*`
   works with ACPI and software nodes too (and makes host testing possible).
7. **Wrong `#address-cells`/`#size-cells`** for `reg`: dtc warns, the
   kernel maps the wrong address. I2C children: `<1>`/`<0>`.
8. **Dereferencing `__iomem` pointers** or using `memcpy` on MMIO: use
   `readl`/`writel`/`memcpy_fromio`. sparse (`make C=1`) catches it.
9. **`writel_relaxed()` before a DMA kick-off**: the device can see stale
   descriptors on arm64.
10. **Overlay without `-@`** or a base DTB without symbols: U-Boot `fdt
    apply` fails ("FDT_ERR_NOTFOUND"); the board falls back or does not boot.
11. **Overlay replacing a list** (`pinctrl-0` of an existing node) instead
    of adding to it — see §3.8.
12. **`platform_device_register()` of a static `struct platform_device`**
    without a `release` → "does not have a release() function" WARN on
    unregister. Use `platform_device_alloc()`/`_register_full()`.
13. **Holding the drvdata of another device without a device link** — it
    is freed when that device unbinds.
14. **Hard-coding addresses or IRQ numbers in the driver** instead of
    reading resources: the next SoC revision moves them.

---

## 8. Files in this lesson

| File | What it shows | Build / run |
|---|---|---|
| [kernel/Makefile](kernel/Makefile) | Kbuild makefile: `KDIR=`, `ARCH=`/`CROSS_COMPILE=`, `W=1`, `clean`, plus `make dtbo` (plain `dtc -@`) | `make drivers` |
| [kernel/platdev_host_demo.c](kernel/platdev_host_demo.c) | **PC or board, no DT needed**: software-node tree (properties + 2 children, `PROPERTY_ENTRY_*`), `platform_device_alloc` + resources (MEM on a private fake bus) + `device_add_software_node` + `platform_device_add`; driver matched by `platform_device_id`; `device_property_*`, `device_for_each_child_node`, `platform_get_resource`, `platform_get_irq_optional`, register map with `readl`/`writel` + `FIELD_PREP`/`FIELD_GET`, devm actions with release-order check, sysfs `dev_groups` (`label`, `ctrl_mode` RW), unbind/rebind self-test, void `.remove` compat | `insmod platdev_host_demo.ko` |
| [kernel/esh_demo_driver.c](kernel/esh_demo_driver.c) | **board** DT driver `esh,demo-device` (`of_match_table` + `MODULE_DEVICE_TABLE(of)` and a `platform_device_id` table), properties/children/sysfs; consumer `esh,demo-consumer` following a phandle with `fwnode_find_reference` + `bus_find_device_by_fwnode`, **deferred probe**, `device_link_add`; `fake_devices=1` builds the same devices from software nodes for a PC/VM | board: `insmod esh_demo_driver.ko` (overlay applied); VM: `insmod esh_demo_driver.ko fake_devices=1` |
| [kernel/esh-demo-overlay.dtso](kernel/esh-demo-overlay.dtso) | **board** overlay: `esh-demo` (label, u32, u32 array, 2 children, status), `esh-demo-consumer` (phandle), `esh-demo-off` (`status = "disabled"`) | `make -C …/kernel dtbo` |
| [user/devmodel_sysfs.c](user/devmodel_sysfs.c) | user-space view: sysfs path, `driver` and `of_node`/`software_node` links, `modalias`, driver attributes (writes `ctrl_mode` as root, checks `-ERANGE`), the consumer's device link, the overlay's node in `/proc/device-tree`, the disabled node has no device | `make drivers`, then `./build/linux-device-drivers/06-device-model-and-device-tree/user/devmodel_sysfs` |

**Build** (from the repo root):

```sh
make drivers                                   # host: modules + user/ programs
make -C linux-device-drivers/06-device-model-and-device-tree/kernel W=1   # extra warnings
make -C linux-device-drivers/06-device-model-and-device-tree/kernel dtbo  # overlay (needs dtc)
make drivers KDIR=<board kernel tree> ARCH=arm64 CROSS_COMPILE=<its toolchain prefix>   # board
                                               # (see ../README.md#building for the exact paths)
make drivers-clean                             # remove *.ko, *.o, .*.cmd, *.dtbo …
```

All modules are **build-tested only**: on the host (Ubuntu 22.04, kernel
6.8, zero warnings with and without `W=1`; checkpatch-clean apart from the
file-name-in-header note this repo's conventions require and the
`LINUX_VERSION_CODE` notes of the `.remove` compatibility guard) and
cross-built against the board's `6.18.x-arm64-k3` PREEMPT_RT tree with its
own GCC 15. The overlay compiles with `dtc -@` (host dtc 1.6.1 and the
kernel's `scripts/dtc`) and applies cleanly to the board's base DTB with
`fdtoverlay`. Loading the modules is **your** step, on the board or in a
VM — see [../README.md](../README.md#safety).

**Run** — PC/VM, no device tree:

```sh
sudo insmod platdev_host_demo.ko
sudo dmesg | grep platdev
./devmodel_sysfs                     # sudo: also tests writing ctrl_mode
sudo rmmod platdev_host_demo

sudo insmod esh_demo_driver.ko fake_devices=1
sudo dmesg | grep esh
sudo rmmod esh_demo_driver           # "consumer removed" comes BEFORE "provider removed"
```

Expected output (illustrative: the modules are build-tested only; numbers
and IRQ/addresses vary):

```
platdev-demo platdev-demo.0: probe: matched by platform_device_id, fwnode platdev-demo
platdev-demo platdev-demo.0:   child channel@0: reg=0 label=adc0
platdev-demo platdev-demo.0:   child channel@1: reg=1 label=adc1
platdev-demo platdev-demo.0: probed: label=host-demo freq=1000000 Hz div=48 thresholds=3 children=2 regs@0x0000000040001000
platdev_host_demo: PASS probe       platdev-demo.0 bound to driver platdev-demo
platdev_host_demo: PASS properties  label=host-demo esh,frequency-hz=1000000 esh,thresholds=<100 200 300> via device_property_*()
platdev_host_demo: PASS children    2 child nodes found with device_for_each_child_node()
platdev_host_demo: PASS resources   MEM 0x0000000040001000 size 0x0000000000000100, platform_get_irq_optional() = -6 (no IRQ)
platdev_host_demo: PASS registers   CTRL=0x00300031: EN=1 MODE=3 DIV=48 (48 MHz / 1 MHz)
platdev-demo platdev-demo.0: remove: device disabled; devm releases follow
platdev_host_demo:   devm release C
platdev_host_demo:   devm release B
platdev_host_demo:   devm release A
platdev_host_demo: PASS unbind      remove() ran, devm released in order CBA (reverse of A,B,C)
platdev-demo platdev-demo.0: probe: matched by platform_device_id, fwnode platdev-demo
...
platdev_host_demo: PASS rebind      device_attach() = 1, probe count 2
platdev_host_demo: done: all checks passed; device stays bound until rmmod

esh-demo-consumer esh-demo-consumer: provider not bound yet -> -EPROBE_DEFER
esh-demo esh-demo:   channel reg=0 label=adc0
esh-demo esh-demo:   channel reg=1 label=adc1
esh-demo esh-demo: provider probed (software node): label=fake-demo frequency=1000000 Hz thresholds=3 channels=2
esh-demo-consumer esh-demo-consumer: consumer probed after 1 deferral(s): provider esh-demo label=fake-demo
esh_demo_driver: PASS provider   bound 1 time(s)
esh_demo_driver: PASS deferred   consumer deferred 1 time(s), then bound 1 time(s)
esh_demo_driver: done: all checks passed
```

**Run** — board, with the overlay (§3.8 for installing it):

```sh
ls /proc/device-tree/ | grep esh            # esh-demo  esh-demo-consumer  esh-demo-off
ls /sys/bus/platform/devices/ | grep esh    # esh-demo  esh-demo-consumer   (no esh-demo-off!)
sudo insmod esh_demo_driver.ko              # or: install it, and udev autoloads it at boot
sudo dmesg | grep esh                       # same as above, "(device tree)", label=board-demo
cat /sys/bus/platform/devices/esh-demo/{label,frequency_hz,thresholds}
cat /sys/bus/platform/devices/esh-demo-consumer/provider
./devmodel_sysfs
dtc -I fs -O dts /sys/firmware/devicetree/base 2>/dev/null | grep -A12 'esh-demo {'
sudo rmmod esh_demo_driver
```

Any `FAIL` line, a `done: N check(s) FAILED`, or any kernel
`BUG:`/`WARNING:` splat means something is wrong. The modules always
return 0 from init so you can `rmmod` them even after a FAIL.

---

## 9. Exercises

**Easy**
1. On the board run `dtc -I fs -O dts /sys/firmware/devicetree/base` and
   find `main_uart0`, the eeprom and the LEDs from §3.6. Which properties
   did the bootloader add or change compared with the source DTS?
2. Remove `label` from the overlay's `esh-demo` node. What does probe print,
   and what is in `/sys/kernel/debug/devices_deferred`? (Trick: nothing —
   why?)
3. Change `esh-demo-off` to `status = "okay"`. How many provider probes do
   you get now, and what is the second device called?
4. `modinfo esh_demo_driver.ko` — find all six aliases and explain each.

**Medium**
5. Make the consumer's dependency visible to fw_devlink: replace the custom
   `esh,provider` with something fw_devlink understands and watch the
   deferral disappear (the consumer is not even tried before the
   provider). Then read `/sys/class/devlink/`.
6. Add an `.data` pointer to the `of_device_id` entries for two variants
   (`esh,demo-device-v2` with a different default frequency) and use
   `device_get_match_data()`.
7. Write the YAML binding `Documentation/devicetree/bindings/misc/esh,demo-device.yaml`
   (required: `compatible`, `label`; `esh,frequency-hz` 1..48000000;
   `esh,thresholds` max 8 items; `channel@N` children) and run
   `make dt_binding_check DT_SCHEMA_FILES=...` in the board tree.
8. In `platdev_host_demo`, protect the `ctrl_mode` read-modify-write with a
   mutex and explain the race it fixes (two `echo`s at once).

**Hard**
9. Give `platdev_host_demo`'s device an IRQ resource from `irqsim_demo`'s
   software interrupt controller (lesson 04) and request it with
   `devm_request_irq(platform_get_irq(pdev, 0))`.
10. Turn `esh_demo_driver` into a *bus*-like provider: make the provider
    create a child platform device per `channel@N` node
    (`of_platform_populate()` on the board, `platform_device_register_full`
    with the child fwnode on the PC) and write a second driver that binds
    to the channels.
11. Port the GPIO button of lesson 04 to read an **I2C GPIO expander** on
    `bone_i2c_2` (P9_19/P9_20) via an overlay with the expander node and a
    `button-gpios` phandle to it. Why does the button driver now defer
    until the expander driver loads, and why must it use the `_cansleep`
    accessors?

---

## 10. Remember forever

```
bus + device + driver; bus->match (DT compatible > ACPI > id_table > name) -> probe()
probe: read props, get resources, map regs, check ID, register; return dev_err_probe(dev, err, ...)
-EPROBE_DEFER = "supplier missing, retry me"; see /sys/kernel/debug/devices_deferred
devm_*: released after remove(), in REVERSE order; add cleanup actions before devm_request_irq
platform: platform_get_resource / devm_platform_ioremap_resource / platform_get_irq
MMIO: readl/writel (ordered vs DMA) vs *_relaxed; FIELD_PREP/FIELD_GET with GENMASK
DT: compatible, reg (parent #address/#size-cells), interrupts, phandles, status="okay", pinctrl-0
MODULE_DEVICE_TABLE(of, ...) -> modalias -> udev autoload; device_property_* is firmware-agnostic
overlay: /plugin/; &{/} or &label; dtc -@; U-Boot fdtoverlays / fdt apply; check: dtc -I fs
.remove returns void since 6.11 (.remove_new before)
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Name the three core structures of the device model and what each owns.
   Which callback decides that a driver fits a device?
2. In what order does the platform bus try to match? How do you force a
   specific driver onto a device from user space?
3. Walk through what happens between `platform_device_add()` and your
   `probe()` being called, and what happens when probe returns
   `-EPROBE_DEFER`.
4. What does `dev_err_probe()` do differently from `dev_err()`?
5. What is fw_devlink, which properties does it understand, and what does a
   device link guarantee at unbind and suspend?
6. In what order are devm resources released? Give a bug caused by that
   order and two ways to fix it.
7. What does `devm_platform_ioremap_resource()` do in two steps? What
   breaks if you skip the first?
8. `readl()` vs `readl_relaxed()`: what is the difference on arm64, and
   when is the relaxed version wrong?
9. Explain `#address-cells`/`#size-cells` and decode
   `reg = <0x00 0x02800000 0x00 0x100>` and an I2C `reg = <0x50>`.
10. What does `status = "disabled"` do, and why do SoC `.dtsi` files use it
    everywhere?
11. How does a module get loaded automatically when its DT node appears?
    Name every piece from `MODULE_DEVICE_TABLE` to `modprobe`.
12. Why prefer `device_property_read_u32()` over `of_property_read_u32()`?
    What are software nodes for?
13. What is a DT overlay, what does `dtc -@` add, and why must the base DTB
    also be built with symbols? How is an overlay applied on the BeagleBone
    AI-64?
14. Trace `main_uart0` from `k3-j721e-main.dtsi` through the board DTS to
    `/dev/ttyS2`. Which property decided the number 2?
15. How do you check, on a running board, that your overlay node really is
    in the tree the kernel uses?
