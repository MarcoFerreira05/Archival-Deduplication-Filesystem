#!/bin/bash
sudo ./passthrough /mnt/fs -omodules="subdir,subdir=/backend" -oallow_other -f
