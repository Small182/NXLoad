# NXLoad

[Українська](#українська) · [English](#english) · [Завантажити / Download](https://github.com/Small182/NXLoad/releases/latest)

## Українська

**NXLoad** — homebrew-каталог і менеджер завантажень для Nintendo Switch. Є пошук, вибране, скриншоти, черга завантажень, розпакування RAR і ZIP та встановлення сумісних NSP на microSD. Інтерфейс доступний українською й англійською. Інший проєкт автора — [UAPlay для Nintendo Switch](https://github.com/Small182/UAPlay-nx).

**Встановлення.** Завантаж `NXLoad.nro` з [Releases](https://github.com/Small182/NXLoad/releases/latest), скопіюй у `switch/` на microSD та відкрий у Homebrew Menu. Завантажені файли зберігаються в `switch/nxload/downloads/`.

**Оновлення.** За замовчуванням NXLoad перевіряє стабільні GitHub Releases без токена. Щоб отримувати збірки з комітів гілки `main`, збережи fine-grained токен із дозволом **Actions: Read** для цього репозиторію у `switch/nxload/github-token.txt` на microSD. Після підтвердження оновлення встановлюється автоматично, коли черга завершена.

**Файли.** Збірки з комітів додають до спільного каталогу ROMsNS. На вкладці «Файли» показуються доступні розміри, зокрема розміри, отримані з метаданих DLArchive. Для Homebrew Ports дія «Розпакувати й встановити» переносить розпакований вміст папки `switch` на microSD і встановлює пакет. Вбудований інсталятор у збірках з комітів також читає сумісні XCI з одним набором метаданих. Для образів, які потребують адаптації під встановлення на microSD, відкрий DBI кнопкою ZL у списку збережених файлів.

**Медіа.** У збірках з комітів вкладка «Медіа» показує скриншоти й пропонує трейлери, коли для гри знайдено відео в американському Nintendo Store. Натисни X або торкнися кнопки «Трейлери», щоб відкрити відео на весь екран.

**Джерела та запозичення.** Повний перелік каталогів, джерел метаданих, файлових сервісів та бібліотек наведено нижче у [спільному розділі](#джерела--credits).

## English

**NXLoad** is a homebrew catalog and download manager for Nintendo Switch. It includes search, favorites, screenshots, a download queue, RAR and ZIP extraction, and installation of supported NSP packages to microSD. The interface is available in English and Ukrainian. See also the author's [UAPlay for Nintendo Switch](https://github.com/Small182/UAPlay-nx).

**Installation.** Download `NXLoad.nro` from [Releases](https://github.com/Small182/NXLoad/releases/latest), copy it to `switch/` on the microSD card, and open it from the Homebrew Menu. Downloads are saved in `switch/nxload/downloads/`.

**Updates.** NXLoad checks stable GitHub Releases by default and needs no token for that channel. To get commit builds from the `main` branch, save a fine-grained GitHub token with **Actions: Read** for this repository to `switch/nxload/github-token.txt` on the microSD card. After you confirm an update, it installs automatically when the queue finishes.

**Files.** Commit builds include ROMsNS in the merged catalog. The Files tab displays available download sizes, including sizes returned by DLArchive metadata. For Homebrew Ports, “Extract and install” moves the extracted contents of the `switch` folder to the microSD card and installs the package. The built-in installer in commit builds also reads compatible XCI images with a single metadata set. For images that require adaptation for microSD installation, press ZL in the saved files list to open DBI.

**Media.** In commit builds, the Media tab displays screenshots and offers trailers when the US Nintendo Store has a video for the game. Press X or tap Trailers to start full-screen playback.

## Джерела / Credits

| Джерело / Source | Використання / Use |
| --- | --- |
| [NSRoms](https://nsroms.net/) · [ROMsHQ](https://romshq.com/) · [ROMsNS](https://romsns.com/) | Каталоги, обкладинки й список файлів. ROMsHQ також надає галереї та вбудований знімок каталогу. / Catalogs, covers and file listings. ROMsHQ also supplies galleries and the bundled catalog snapshot. |
| [DLArchive](https://drive.dlarchive.my.id/) · [DataNodes](https://datanodes.to/) · [1fichier](https://1fichier.com/) · [MegaUp](https://megaup.net/) · [NSWGF](https://nswgf.com/) | Файлові хости, дзеркала та обробники посилань у коді. / File hosts, mirrors, and source-specific link handlers in the codebase. |
| [Nintendo Life](https://www.nintendolife.com/games) | Метадані, оцінки читачів, обкладинки й скриншоти. Вбудовані галереї 1-2-Switch та MySims Cozy Bundle використовують зображення Nintendo Life. / Metadata, reader ratings, covers and screenshots. The bundled 1-2-Switch and MySims Cozy Bundle galleries use Nintendo Life images. |
| [TitleDB](https://github.com/blawar/titledb) · [Nintendo eShop](https://www.nintendo.com/store/games/) | Ідентифікатори, описи, жанри, дати, відомості про студії, зображення та відео з галерей Nintendo Store. / Identifiers, descriptions, genres, dates, studio information, and images and videos from Nintendo Store galleries. |
| [devkitPro](https://devkitpro.org/) · [libnx](https://github.com/switchbrew/libnx) · [Homebrew Menu](https://github.com/switchbrew/nx-hbmenu) | Інструменти збірки, системні API та запуск NRO. / Build toolchain, system APIs, and NRO launcher. |
| [SDL2](https://github.com/libsdl-org/SDL) · [SDL_image](https://github.com/libsdl-org/SDL_image) · [SDL_ttf](https://github.com/libsdl-org/SDL_ttf) | Інтерфейс, зображення та текст із системним шрифтом Switch. / UI, image decoding, and text with the Switch system font. |
| [mpv](https://github.com/mpv-player/mpv) · [FFmpeg](https://ffmpeg.org/) · [devkitPro portlibs](https://github.com/devkitPro/pacman-packages/tree/master/switch) | Відтворення трейлерів у збірках з комітів. / Trailer playback in commit builds. |
| [curl](https://curl.se/) · [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) · [Mozilla CA bundle](https://curl.se/docs/caextract.html) | HTTPS, TLS і набір кореневих сертифікатів Mozilla, який поширює curl. / HTTPS, TLS, and the Mozilla root certificates distributed by curl. |
| [libxml2](https://gitlab.gnome.org/GNOME/libxml2) · [libarchive](https://www.libarchive.org/) · [zlib](https://zlib.net/) | Розбір HTML, розпакування RAR і ZIP, стиснені метадані. / HTML parsing, RAR and ZIP extraction, and compressed metadata. |
| [Switchbrew](https://switchbrew.org/wiki/XCI) | Документація формату XCI. / XCI format documentation. |
| [DBI](https://github.com/rashevskyv/dbi) (duckbill) · [GitHub Actions](https://github.com/features/actions) | DBI можна відкрити як окремий інсталятор, якщо він уже є на microSD. GitHub Actions збирає NRO. / DBI can be opened as a separate installer when already present on microSD. GitHub Actions builds the NRO. |
| [OpenAI](https://openai.com/) | Підготовка українських перекладів описів в окремому процесі збірки. / Ukrainian game descriptions are prepared in a separate translation workflow. |

Зображення, назви ігор і торговельні марки належать відповідним правовласникам. / Game images, titles, and trademarks belong to their respective owners.

## Збірка / Build

Потрібні Python 3, devkitA64, libnx, switch-tools, switch-pkg-config та Switch portlibs для libmpv, SDL2, SDL2_image, SDL2_ttf, libxml2, libarchive і zlib. / Requires Python 3, devkitA64, libnx, switch-tools, switch-pkg-config, and the corresponding Switch portlibs.

```sh
bash tools/build-network.sh
bash tools/build-archive.sh
make
```
