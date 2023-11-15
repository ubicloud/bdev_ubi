# bdev_ubi

## Introduction

bdev_ubi provides an SPDK virtual bdev layered over another bdev, enabling
copy-on-access for a base image.

This can be utilized to set up a block device initialized with an image file,
without the delay of an initial copy. With bdev_ubi, copying occurs lazily upon
block access, rather than at the provisioning time.

For instance, let's say you already have a bdev named "aio0" and you wish to
populate it with data from `/opt/images/large-image.raw`. A conventional
approach would be to copy `large-image.raw` to `aio0` using tools like spdk_dd,
which can be time-consuming. However, with bdev_ubi, you can proceed by invoking
the following json-rpc method:

```
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_ubi_create",
  "params": {
    "name": "ubi0",
    "base_bdev": "aio0",
    "image_path": "/opt/images/large-image.raw",
    "stripe_size_mb": 1
  }
}
```

and rather than directly using `aio0`, use `ubi0` as the block device. This
action completes almost instantly, eliminating the need for initial data
copying. The actual data copying will occur lazily during I/O requests.

## Building

Install some requirements:

```
sudo apt update
sudo apt install pkg-config build-essential liburing-dev
```

Clone and build SPDK:

```
git clone https://github.com/spdk/spdk
cd spdk
git submodule update --init
sudo scripts/pkgdep.sh
./configure --with-crypto --with-vhost
make -j16
```

Build bdev_ubi:

```
SPDK_PATH=/path/to/spdk/build/ make
```

## Usage

The steps in the previous section generates an SPDK app in
`build/bin/vhost_ubi`. The application can be managed through JSON-RPC or by
providing block device (bdev) configuration via command-line arguments,
analogous to the functionality offered by the SPDK's standard `vhost`
application.

For instance, begin by downloading and preparing the Ubuntu Jammy image, which
will be used as the base read-only image:

```
wget https://cloud-images.ubuntu.com/jammy/current/jammy-server-cloudimg-amd64.img
qemu-img convert -p -f qcow2 -O raw jammy-server-cloudimg-amd64.img jammy.raw
```

Next, generate the file that will function as the writable layer for the block
device:

```
touch write-space
truncate -s 5G write-space
```

Next, reserve 1G of hugepages:

```
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

Finally, start the app with the configuration provided in
`examples/spdk_conf.json`:

```
sudo build/bin/vhost_ubi --json examples/spdk_conf.json -S /var/tmp
```

What will happen is:
* An aio bdev called `aio0` will be created pointing to `write-space`.
* A ubi bdev called `ubi0` will be created, with base image `jammy.raw` and base
  bdev `aio0`.
* A vhost-user-blk controller called `vhost.0` will be created, which will be
  bound to the UNIX domain socket `/var/tmp/vhost.0`.

Now you can start a VM using the vhost disk:

```
sudo cloud-hypervisor \
    ...
    --disk vhost_user=true,socket=/var/tmp/vhost.0,num_queues=4,queue_size=256 \
    ...
```

## JSON-RPC API

### bdev_ubi_create

Parameters:
* `name` (text, required): Name of the bdev to be created.
* `image_path` (text, required): Path to the image file.
* `base_bdev` (text, required): Name of base bdev.
* `stripe_size_mb` (integer, optional): Stripe size in megabytes. Defaults to 1.

**Note.** When creating the bdev for the first time, magic bits in the metadata
section of base image should be zeroed. For unencrypted base bdev, truncate
command in the previous section will take care of this. For encrypted base bdev,
`spdk_dd` can be used with parameters `--bs 512 --count 1 --if /dev/zero --ob
[ubi_bdev_name]`.

### bdev_ubi_delete

Parameters:
* `name` (text, required): Name of the bdev to be deleted.

## Internals

### Data Layout

First 8MB of base bdev is reserved for metadata. Metadata consists of:
* Magic bytes (9 bytes): `BDEV_UBI\0`
* Metadata version major (2 bytes)
* Metadata version minor (2 bytes)
* stripe_size_mb (1 byte)
* Stripe headers: 4 byte per stripes. Currently it specifies whether a stripe
  has been fetched from image or not. 31-bits are reserved for future extension.
* Padding to make the total size 8MB.

Then at the 8MB offset the actual disk data starts.

### Read/Write I/O operations

If the stripe containing the requested block range hasn't been fetched yet, then
a stripe fetch is enqueued. Once stripe has been fetched, the actual I/O
operation is served.

### Flush (aka sync)

* Data for the requested range is flushed to base bdev.
* Once data flush is finished, and if metadata has been modified in memory, then
  metadata is first written and then flushed to base bdev.
