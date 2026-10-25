/* Internal native/Win32 consumer attachment to the bound display owner.
 * No public ABI, PCI discovery, framebuffer mapping or presentation grant.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/display_driver.h>
#include <ciuki/fbdev.h>
#include "render3d.h"
int render3d_native_init(struct render3d_service *s,const struct render3d_host *host)
{
    if (!s || !host || !host->alloc || !host->free || !host->now_us) return -EINVAL;
    const struct display_driver *driver=display_driver_bound();
    const struct fb_device *fb=fbdev_get();
    if (!fb || !fb->present || !fb->generation) return -ENODEV;
    const struct render3d_family *family=driver ? driver->render3d : 0;
    const struct render3d_backend *b=family && family->status==R3D_UNQUALIFIED && family->backend ? family->backend() : 0;
    if (b && b->attach) {
        int e=b->attach(s,host,fb->generation);
        if (!e) return 0;
        if (e!=-EOPNOTSUPP && e!=-ENODEV) return e;
    }
    render3d_init(s,host,fb->generation,0,0,(struct render3d_extent){0});
    return 0;
}
