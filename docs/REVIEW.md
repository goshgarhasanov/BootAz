# BootAz 1.0 — kod, test, təhlükəsizlik və gələcək review

Developed By Goshgar Hasanzadeh · 16 oktyabr 2026

## 1. Testlər

| Səviyyə | Nə yoxlanılır | Nəticə |
|---|---|---|
| Unit (`bootaz-tests`) | MD5/SHA-1/SHA-256/SHA-512 test vektorları, UTF-8/16, ISO 9660 + Rock Ridge + Joliet, UDF (Windows tipli), El Torito, 4 sıxılma formatı, zərərli ISO adları | 58/58 |
| Tərcümə (`check_i18n.py`) | 400+ mətnin hamısının azərbaycanca qarşılığı və `%1` yer tutucuları | 0 əskik |
| GUI ↔ CLI (`bootaz-engine-test`) | GUI-nin iş mühərriki CLI-ı işə salır, progress gəlir, iş bitir, nəticə GPT + FAT32-dir | keçdi |
| İnteqrasiya (`sudo tests/integration.sh`) | Loop cihazlarında DD, ISO GPT/MBR, Syslinux, Windows tipli NTFS + UEFI:NTFS + unattend, 4 fayl sistemi, bərpa, saxta disk (device-mapper), bad blocks, saxlama, sistem diskinin qorunması, real Windows 11 ISO ilə 3 düzülüş | 32/32 |
| QEMU-da yükləmə | Alpine: DD (BIOS + UEFI), ISO GPT (UEFI), ISO MBR Syslinux və GRUB (BIOS + UEFI). Windows 11 (Linux-dan yazılmış): NTFS + UEFI:NTFS (UEFI + BIOS), FAT32 + NTFS (UEFI, **masaüstünə qədər tam quraşdırma**, lokal hesab, MS hesabı və TPM olmadan), FAT32 + bölünmüş WIM (UEFI + BIOS) | hamısı açıldı |
| Windows build | MinGW ilə cross-compile, Wine altında analiz, ISO rejimi, hash | Linux ilə eyni nəticə |

Tutulan və düzəldilən xətalar: `linux` makrosu ilə ad toqquşması; Joliet test obrazı; `isolinux.bin`-in qeyri-standart yeri; etiketin sonundakı boşluq; saxta disk aşkarlanmasında wrap-around istiqaməti; `--win-layout split`-in fayl sistemi seçimindən sonra oxunması; CLI-ın JSON rejimində bağlanmaması (GUI "işləyir"də qalardı); `blkid` keşi (test); UEFI:NTFS bölməsinin "RUFUS_BOOT" adı; Windows obrazında GUI kartlarının sıxılması.

## 2. Kod review

- **Arxitektura**: nüvə sırf C++20 (Qt yoxdur) → CLI və GUI eyni kodu işlədir, Windows üçün cross-compile olunur. Platformaya xas olan yalnız `Disk_linux.cpp` / `Disk_win.cpp`.
- **İmtiyaz ayrılığı**: GUI adi istifadəçi kimi işləyir; yalnız yazma işi `pkexec bootaz-cli` ilə root olur. Plan GUI-də hesablanır ki, problemlər parol soruşulmamış görünsün.
- **Windows backend** üçün review-da tapılan və düzəldilənlər: FAT32 + BIOS yolunda volume-un iki dəfə kilidlənməsi, UEFI:NTFS bölməsinin kilidsiz yazılması, paralel FormatEx çağırışlarında qlobal vəziyyət yarışı (indi mutex var).
- **Məlum məhdudiyyətlər**: Windows-da raw disk yazma yolu yalnız kompilyasiya və Wine ilə yoxlanılıb, real Windows + USB-də sınaq lazımdır. Windows-da ext2/3/4 formatı və persistence yoxdur. ISO rejimində Linux üçün BIOS yükləməsi host-dakı syslinux/grub-install-dan asılıdır (yalnız Linux-da).

## 3. Təhlükəsizlik review

| Risk | Vəziyyət |
|---|---|
| Sistem diskinə yazmaq | `/`, `/boot`, `/boot/efi`, `/usr`, `/var`, `/home`, swap, LVM/dm və RAID altındakı disklər tapılır və yazılmır; Windows-da Windows qovluğunun diski. GUI onları seçilməz göstərir |
| Səhv diskə yazmaq (hotplug) | Disk seçiləndə onun ID-si (seriya / ölçü) götürülür və yazmadan dərhal əvvəl yenidən yoxlanılır; Linux-da `O_EXCL` |
| Zərərli ISO ilə fayl sisteminə yazmaq (path traversal) | **Düzəldildi**: Rock Ridge / Joliet / UDF adlarında `..`, `/`, `\`, idarə simvolları rədd edilir; test: `evil.iso` |
| Zərərli ISO ilə yaddaşı doldurmaq | **Düzəldildi**: maksimum 2 milyon giriş, qovluq dərinliyi 64, AD zəncirləri məhdudlaşdırılıb |
| /tmp symlink hücumu (root) | **Düzəldildi**: müvəqqəti qovluqlar `mkdtemp` ilə (Windows-da `CreateDirectory` mövcud qovluğu rədd edir) |
| GUI vasitəsilə root kimi istənilən fayla yazmaq (`--allow-file`, `save --out`) | **Düzəldildi**: pkexec ilə işləyəndə hədəf fayl/qovluq istifadəçinin özünə məxsus olmalıdır; symlink rədd edilir; `chown` yalnız root-un yeni yaratdığı adi fayla |
| XML / əmr inyeksiyası (`autounattend.xml`) | İstifadəçi dəyərləri XML-escape olunur; hesab adı, kompüter adı və regional format ciddi yoxlanılır; `net user` əmri yalnız yoxlanılmış adla |
| Yükləmələr | Yalnız HTTPS, təhlükəsiz redirect, rəsmi SHA-256 ilə yoxlama; serverdən gələn fayl adı təmizlənir və `.iso` olmalıdır |
| Terminal / JSON çıxışı | JSON-da bütün sətirlər escape olunur; xarici proqram yalnız `execv` ilə, shell olmadan |
| Telemetriya | Yoxdur. İnternet yalnız "Obraz yüklə" pəncərəsində |

## 4. Gələcək review (növbəti versiyalar üçün)

Prioritet sırası ilə:

1. **Real Windows aparatında sınaq** (USB 2/3, müxtəlif fləşlər) və Windows üçün imzalı installer (Inno Setup / MSIX).
2. **Ventoy tipli multiboot rejimi**: bir fləşdə bir neçə ISO, yükləmə menyusu (rus və ingilis mənbələrində ən çox istənilən funksiya).
3. **Windows To Go**: Windows-u birbaşa fləşə quraşdırmaq (wimlib apply + BCD).
4. **Secure Boot DBX / SBAT yoxlaması**: yükləyicinin ləğv olunmuş siyahıda olub-olmadığını Microsoft-un DBX faylı ilə yoxlamaq (Rufus 4.6+ kimi).
5. **Microsoft-dan pərakəndə Windows ISO yükləmək** (Fido kimi), indi yalnız Evaluation və rəsmi səhifə var.
6. **Linux BIOS yükləməsi Windows-dan**: syslinux/GRUB-u daxili etmək (indi host alətindən asılıdır).
7. **Klonlama**: bir fləşi birbaşa digərinə köçürmək (Etcher kimi), VHDX / FFU saxlama.
8. **El Torito EFI obrazından UEFI faylları** çıxarmaq (yalnız El Torito-da EFI olan ISO-lar üçün ISO rejimi).
9. **Tam ekran oxunaqlılıq**: ekran oxuyucu etiketləri, klaviatura ilə tam idarə, böyük şrift rejimi.
10. **Avtomatik yeniləmə yoxlaması** (GitHub Releases) və rus/türk dili.
11. **Paket**: Flatpak / AppImage / .deb, polkit siyasəti ilə daha aydın parol pəncərəsi.

## 5. Review 2 (1.0.1, 18 oktyabr 2026)

Bütün kod yenidən oxundu; ASan + UBSan ilə build, mutasiya fuzz testi (ISO 9660 / Rock Ridge / Joliet / UDF,
6000+ zədələnmiş obraz), loop cihazlarında inteqrasiya (29/29), QEMU-da Alpine (UEFI + BIOS), Wine altında Windows CLI.

| Tapılan | Təsir | Düzəliş |
|---|---|---|
| Zərərli ISO: ad uzunluğu qeyddən böyük olanda yaddaşdan kənar oxuma və `std::length_error` (fuzz tapdı) | GUI çökə bilərdi | Qeyd sərhədləri yoxlanılır; UDF partition map sərhədi; `planWrite` hər istisnanı tutur |
| `fsync` xətası nəzərə alınmırdı | Çıxarılan / xarab fləşdə "uğurlu" yazı | Xəta indi işi dayandırır |
| `fork()`-dan sonra yaddaş ayrılması (bir neçə fləş paralel) | Nadir donma | `argv` / mühit `fork`-dan əvvəl hazırlanır, `execve`; alətlərin stdin-i `/dev/null` (ləğv kanalı qorunur) |
| Bir neçə fləşə yazanda və alətlərdə (bad blocks, saxta disk, bərpa) disk ID yoxlanılmırdı | Hotplug-da səhv diskə yazmaq riski | Hər `--device` üçün öz `--expect-id`-si; eyni disk iki dəfə verilsə bir dəfə yazılır |
| Bölünmüş WIM üçün müvəqqəti fayllar (~9 GB) xətada qalırdı və RAM-dakı `/tmp`-yə yazılırdı | Disk / RAM dolur | RAII təmizləmə; boş yer yoxlanılır, `/var/tmp` üstünlüklüdür |
| Sistem diski yalnız `/proc/mounts`-dakı adla tapılırdı | `/dev/root` kimi adlarda qorunma yox | Cihaz nömrəsi ilə də yoxlanılır |
| FAT32: yaradılmış `ADI~1.EXT` ilə eyni adlı real fayl | Təkrar qısa ad | Toqquşma yoxlanılır |
| `--cluster` yanlış dəyər, mənfi ölçü, QEMU-da vergüllü fayl adı | Çökmə / səhv | Yoxlanılır / escape olunur |

Logo: ikonun bütün fonu Azərbaycan bayrağıdır, üstündə ağ USB fləş, ay-ulduz fləşin üzərində.
