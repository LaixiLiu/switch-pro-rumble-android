# Switch Pro Controller hidraw access probe

This is a separate, temporary Magisk test module. It does not modify the existing Xbox rumble module and has no boot service. Its only purpose is to allow the Magisk `su` domain to open and write HID raw character devices so a one-shot Switch rumble probe can be run manually.

## SELinux rule

`sepolicy.rule` grants `magisk` access to `vendor_hidraw_device` character devices. Android SELinux labels these nodes by type, so the rule cannot be restricted to only the currently connected Nintendo controller. The rule can be revoked by disabling or removing this module and rebooting.

The module must be installed and the phone rebooted before the rule applies. No module has been installed and no reboot has been performed.

## Required next validation

After the rule is active, click the module's **Action** button in Magisk. `action.sh` resolves the current Nintendo node dynamically from `/sys/class/hidraw`, sends a roughly 150 ms low-amplitude pulse, and follows it with an explicit stop report. It does not start a background process. Do not rely on a fixed `hidrawN` number because it may change after reconnect or reboot.

The report fields follow Linux `hid-nintendo.c` v6.1: rumble-only report `0x10`, rolling packet number, and two encoded four-byte motor values. The frequency/amplitude bytes are precomputed from the driver's default 160/320 Hz frequencies and a low amplitude setting.
