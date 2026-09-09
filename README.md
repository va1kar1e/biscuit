# Biscuit

09/09/2026 - version `0.1.0-thai-media-ota6`
Forked from [yattsu/biscuit](https://github.com/yattsu/biscuit). All core reading functionality comes from Biscuit.

## Features

- [Added] Read Thai-language EPUBs with the PK Nakhon Sawan font
- [Added] Open BMP, JPG/JPEG, and PNG files from the File Browser
- [Added] `Tools > Image Viewer` for browsing only BMP, JPG/JPEG, and PNG files on the MicroSD
- [Added] Supports entering subfolders, with images in each folder sorted using natural sort order
- [Fixed] Update firmware via `/update.bin` at the root of the MicroSD
- [Fixed] Send `update.bin` to the MicroSD through the File Transfer page over the network
- [Fixed] Improved File Transfer stability: disables Wi-Fi sleep, enables auto-reconnect, and waits for the connection to recover from brief drops before leaving the page
- [Fixed] No screen refresh while reading `update.bin` from the MicroSD
- [Fixed] for Thai book/chapter titles in the status bar

⚠️ **Note:** After installing Biscuit, I found that my device could no longer be updated due to a USB Lock (burnt eFuse).
The link below worked for me to update the firmware again: [Fix Bricked Xteink](https://github.com/paulporto/crosspoint-reader/blob/develop/docs/fix-bricked-xteink.md)

![UpdateFW](./docs/images/updateFW.jpg)

```bash
~$ sudo flashrom --programmer ch341a_spi -r backup_0.bin
    [...]
    Reading flash... done.
~$ sudo flashrom --programmer ch341a_spi -r backup_1.bin
    [...]
    Reading flash... done.
    # lets compare the hashes from the back ups
~$ md5sum backup_0.bin
    211522e56616ea46ac9bcf82d3451eb2  backup_0.bin
~$ md5sum backup_1.bin
    211522e56616ea46ac9bcf82d3451eb2  backup_1.bin
~$ sudo flashrom --programmer ch341a_spi -w crosspoint_backup.bin
    [...]
    Reading old flash chip contents... done.
    Erasing and writing flash chip... Erase/write done.
    Verifying flash... VERIFIED.
```

Backup file [crosspoint_backup.bin](./docs/crosspoint_spiflash_backup.tar.xz)

After installing `crosspoint_backup.bin`, simply drag and drop the custom firmware file onto the root of the SD card, rename it to `update.bin`, then insert the SD card into the XTeink and hold **Power + Vol Up** until it boots into firmware upgrade mode


## Generating Images from PDF

The `pdf_to_img_x4.py` script accepts multiple PDF files, splits each page into 4 parts in manga reading order, and generates 480x800 BMP images:

```bash
python pdf_to_img_x4.py volume1.pdf volume2.pdf -o output
```

Copy the resulting folder to the MicroSD, open the first image, then use the Page Forward / Page Back buttons to continue reading seamlessly.
