# Switch Download Probe

Нативна діагностика завантажень на Nintendo Switch через інтернет із записом на microSD.

## Запуск v0.5

Скопіюй `Switch-Download-Probe.nro` до `switch/` і запусти через Homebrew Menu.

- **R** перевіряє WebApplet на контрольному перенаправленні. Якщо все спрацює, програма отримає адресу, виконає власний GET і запише `switch/SwitchDownloadProbe/webapplet-test.json`.
- **L** відкриває MegaUp для My Cozy Room в системному браузері. Натисни справжню кнопку завантаження на сторінці й дочекайся штатного таймера. Якщо WebApplet повертає адресу на `e13.megaupdownup.org`, програма виконує власний GET і зберігає файл на SD.
- **ZL** дає ввести іншу адресу `https://megaup.net/` для того самого браузерного тесту. Збірка очікує перенаправлення на `e13.megaupdownup.org`, який був у попередньому тесті. Інший файловий домен не дасть того самого callback.
- **X** запускає попередній тест A Pretty Odd Bunny через NSWGF та 1Fichier. **Y** приймає адресу 1Fichier вручну. **A** запускає перевірку Buzzheavier.
- **B** скасовує фонову передачу або повертає з WebApplet. **+** закриває програму.

Готові файли зберігаються в `switch/SwitchDownloadProbe/`. Підписана адреса MegaUp використовується в пам'яті для одного завантаження.

## Збірка

У devkitPro потрібні devkitA64, libnx, switch-tools, switch-curl, switch-zlib і switch-libxml2. Запусти `make`. GitHub Actions зберігає готовий NRO як артефакт.
