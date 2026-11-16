# Switch Download Probe

Нативна діагностика завантажень на Nintendo Switch через інтернет із записом на microSD.

## MegaUp Downloader 0.1

`MegaUp-Downloader.nro` запускається окремо через Homebrew Menu. Застосунок відкриває MegaUp через HTTPS на самій консолі та зберігає файл у `switch/MegaUpDownloader/` на microSD.

1. Скопіюй `MegaUp-Downloader.nro` у папку `switch/` на картці.
2. Натисни **A** та введи адресу сторінки `https://megaup.net/…`. Можна також записати адресу в перший рядок `switch/MegaUpDownloader/link.txt` і натиснути **X**.
3. Під час завантаження **B** скасовує передачу. Після успішного запису тимчасовий файл `.part` перейменовується у фінальний.

Екран показує етап і HTTP-статус, якщо MegaUp повертає помилку. Якщо сервер вимагає браузерну перевірку, завантажувач повідомить про це. Для перевірки підійде чинне посилання на невеликий власний файл.

Окремий NRO збирається командою `make MEGAUP_ONLY=1`. В Actions він доступний в артефакті **MegaUp-Downloader**.

## Запуск Switch Download Probe v0.5

Скопіюй `Switch-Download-Probe.nro` до `switch/` і запусти через Homebrew Menu.

- **R** перевіряє WebApplet на контрольному перенаправленні. Якщо все спрацює, програма отримає адресу, виконає власний GET і запише `switch/SwitchDownloadProbe/webapplet-test.json`.
- **L** відкриває MegaUp для My Cozy Room в системному браузері. Натисни справжню кнопку завантаження на сторінці й дочекайся штатного таймера. Якщо WebApplet повертає адресу на `e13.megaupdownup.org`, програма виконує власний GET і зберігає файл на SD.
- **ZL** дає ввести іншу адресу `https://megaup.net/` для того самого браузерного тесту. Збірка очікує перенаправлення на `e13.megaupdownup.org`, який був у попередньому тесті. Інший файловий домен не дасть того самого callback.
- **X** запускає попередній тест A Pretty Odd Bunny через NSWGF та 1Fichier. **Y** приймає адресу 1Fichier вручну. **A** запускає перевірку Buzzheavier.
- **B** скасовує фонову передачу або повертає з WebApplet. **+** закриває програму.

Готові файли зберігаються в `switch/SwitchDownloadProbe/`. Підписана адреса MegaUp використовується в пам'яті для одного завантаження.

## Збірка

У devkitPro потрібні devkitA64, libnx, switch-tools, switch-curl, switch-zlib і switch-libxml2. Запусти `make`. GitHub Actions зберігає готовий NRO як артефакт.
