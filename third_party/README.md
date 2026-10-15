# Third-party components

| Folder | What | Source | License |
|---|---|---|---|
| `ms-sys/` | Boot code for the Windows 7+ MBR, the FAT32 (bootmgr) and NTFS boot records, and the Syslinux MBRs | [ms-sys](https://ms-sys.sourceforge.io/) as shipped in [Rufus](https://github.com/pbatard/rufus/tree/master/src/ms-sys/inc) | GPL-2.0-or-later (ms-sys) / GPL-3.0 (Rufus) |
| `uefi-ntfs/uefi-ntfs.img` | The small FAT partition that lets UEFI firmware boot NTFS and exFAT drives (Secure Boot signed NTFS drivers) | [Rufus res/uefi](https://github.com/pbatard/rufus/tree/master/res/uefi), built from [uefi-ntfs](https://github.com/pbatard/uefi-ntfs), [ntfs-3g](https://github.com/pbatard/ntfs-3g) and [EfiFs](https://github.com/pbatard/efifs) | GPL-2.0-or-later / GPL-3.0 |

BootAz itself is GPL-3.0, so these can be used as they are. Thanks to Pete Batard and the
ms-sys, ntfs-3g and EfiFs authors.
