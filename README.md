# Switch Download Probe

Діагностика HTTPS на Nintendo Switch для My Cozy Room на Buzzheavier та 1Fichier.

## Запуск

1. Скопіюй `Switch-Download-Probe.nro` до папки `switch/` на microSD.
2. Запусти програму в Homebrew Menu з підключеним Wi-Fi.
3. Натисни **A** для Buzzheavier або **X** для 1Fichier. Для виходу натисни **+**.

Перший HTTP-код належить сторінці файлу, другий — дії завантаження. Нуль означає, що етап не відбувся або HTTP-відповідь не надійшла. Код 403 із повідомленням про Cloudflare означає, що сервер запропонував браузерну перевірку.

- **Buzzheavier.** Якщо сторінка надає підписану дію Download File, тест очікує HTTP 204 із заголовком HX-Redirect.
- **1Fichier.** Тест перевіряє сторінку файлу. Якщо вона містить форму Free Download, тест виконує лише перший крок форми й повідомляє, чи 1Fichier повернув наступну форму, адресу або обмеження.

Застосунок не переходить за адресами передачі файлу та не зберігає архіви. URL з підписами й cookies не виводяться.

## Збірка

Встанови групу розробки Nintendo Switch devkitPro, зокрема devkitA64, libnx, switch-tools, switch-curl і switch-zlib. У терміналі devkitPro виконай `make`. Файл `.nro` з'явиться в корені проєкту.
