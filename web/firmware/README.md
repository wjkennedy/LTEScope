# Generated firmware images

The `.bin` and GPL license files in each target directory are generated and
not checked in:

```bash
./firmware/esp32-sdr/build.sh heltec-v3
./firmware/esp32-sdr/build.sh t-display-s3
./firmware/esp32-sdr/build.sh cyd
```

The browser build copies this directory, including the target manifests, into
the web distribution.
