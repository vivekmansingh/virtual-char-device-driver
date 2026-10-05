savedcmd_/mnt/d/virtual-char-device-driver/mydev.mod := printf '%s\n'   mydev.o | awk '!x[$$0]++ { print("/mnt/d/virtual-char-device-driver/"$$0) }' > /mnt/d/virtual-char-device-driver/mydev.mod
