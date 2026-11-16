# Switch Download Probe

Діагностичний Nintendo Switch homebrew для одного прикладу, My Cozy Room на Buzzheavier. Проєкт перевіряє, чи звичайний HTTPS-клієнт консолі отримує від сторінки штатну дію Download File та відповідь HTTP 204 із підписаним HX-Redirect.

## Запуск

1. Скопіюйте Switch-Download-Probe.nro до папки switch/ на microSD.
2. Запустіть програму в Homebrew Menu з підключеним Wi-Fi.
3. Натисніть A та дочекайтеся результату. Для виходу натисніть +.

Перший HTTP-код належить сторінці файлу. Другий належить дії Download File. Нуль означає, що сервер не повернув HTTP-відповіді або що відповідний етап ще не запускався. PASS означає, що сервер видав очікуваний заголовок. Діагностика не відкриває адресу з HX-Redirect та не завантажує файл. Значення підписаних адрес і cookies не виводяться.

## Збірка

Встановіть групу розробки Nintendo Switch devkitPro, зокрема devkitA64, libnx, switch-tools, switch-curl і switch-zlib. У терміналі devkitPro виконайте make. Результат з'явиться в корені проєкту.
