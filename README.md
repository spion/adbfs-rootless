This variant of adbfs works even WITHOUT having root access (busybox) on your phone!

Instructions:
=============

You will need `libfuse-dev` and `adb`. You will also need `build-essential`, `git`, and `pkg-config`. On Ubuntu:
    
    sudo apt-get install libfuse-dev android-tools-adb
    sudo apt-get install build-essential git pkg-config

Clone the repository:

    git clone git@github.com:spion/adbfs-rootless.git
    cd adbfs-rootless    

Build:

    make

Optional: If you have a separate copy of android-sdk and would
like to use that adb, copy the binary adbfs to the `android-sdk/platform-tools`
directory. If platform-tools is in your $PATH you can skip this step.

Create a mount point if needed (e.g. in your home directory):

    mkdir ~/droid

You can now mount your device (also from the platform-tools dir):

    ./adbfs ~/droid

If you want to trigger a media rescan after every operation, use the option `-o rescan`:

    ./adbfs -o rescan ~/droid

Have fun!

## Troubleshooting

### Error: device not found

When running you get the following error:

```
--*-- exec_command: adb shell ls
error: device not found
```

Solution: Make sure that [USB Debugging is enabled][enable-usb-debug].

Then `fusermount -u /media/mount/path` before trying again. Note that if for any reason `fusermount` is not available in your system, you can use `sudo umount /media/mount/path` instead.

### Error: device offline

When running you get the following error:

```
--*-- exec_command: adb shell ls
error: device offline
```

Solution: Make sure that

1. Your android-sdk-tools are up to date. Newer versions
   of Android also require newer versions of adb. For more info, see 
   [this Stack Overflow post][error-device-offline].

2. You answer `Yes` when your phone asks whether it should allow the 
   computer with the specified RSA key to access the device.

Then `killall -9 adb; fusermount -u /media/mount/path` before trying again.


[enable-usb-debug]: http://www.droidviews.com/how-to-enable-developer-optionsusb-debugging-mode-on-devices-with-android-4-2-jelly-bean/
[error-device-offline]: http://stackoverflow.com/questions/10680417/error-device-offline
