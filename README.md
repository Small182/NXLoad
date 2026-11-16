# NXLoad

Нативний каталог Nintendo Switch та завантажувач DLArchive для microSD.

Скопіюй `NXLoad.nro` у `switch/` і запусти через Homebrew Menu.

- **A** відкриває картку гри, а в картці завантажує вибраний файл DLArchive. У списку можуть бути окремі файли базової гри, оновлення й DLC.
- Прокрутка вниз підвантажує наступну сторінку NSRoms. **L / R** перемикають сторінки. **X** відкриває пошук за назвою по всьому каталогу.
- **Y** перемикає екран завантажень. На ньому видно прогрес і вже збережені файли. **X** скасовує активне завантаження.
- **A** на екрані збережених файлів відкриває DBI, якщо він є на карті. У DBI вибери файл у `switch/SwitchDownloadProbe/` через Browse SD Card.
- **B** повертає назад. **+** виходить.

NXLoad читає каталог з NSRoms і отримує актуальне посилання від API DLArchive перед кожним завантаженням. Готові файли зберігаються в `switch/SwitchDownloadProbe/`, тож файли попередньої тестової версії доступні в NXLoad.

## Збірка

Потрібні devkitA64, libnx, switch-tools, switch-pkg-config, switch-sdl2, switch-sdl2_ttf, switch-sdl2_image, switch-curl і switch-libxml2. Запусти `make`. GitHub Actions зберігає готовий `NXLoad.nro` в артефакті `NXLoad`.

Для повторної TLS-перевірки файлового сервера NXLoad містить набір публічних кореневих сертифікатів [Mozilla CA](https://curl.se/docs/caextract.html), ліцензія MPL 2.0.
