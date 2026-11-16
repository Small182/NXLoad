# Switch Download Probe

Нативний .nro для Nintendo Switch. Збірка 0.3 перевіряє потік 1Fichier для My Cozy Room і пише файл напряму на microSD. Для мережевих запитів потрібен інтернет.

## Запуск

1. Скопіюй `Switch-Download-Probe.nro` до папки `switch/` на microSD.
2. Запусти його з Homebrew Menu.
3. Натисни **X** для завантаження з 1Fichier або **A** для діагностики Buzzheavier. **B** зупиняє завантаження, **+** закриває застосунок.

1Fichier працює штатними гостьовими HTTP-запитами. Екран показує очікування, байти й фінальний HTTP-код. Якщо сервіс повертає браузерну перевірку або невідому форму, завдання зупиняється з поясненням. Файл пишеться до `switch/SwitchDownloadProbe/`: розширення `.part` знімається після успішного завершення передачі. Початковий тест виконується без докачування.

## Збірка

У devkitPro потрібні devkitA64, libnx, switch-tools, switch-curl, switch-zlib і switch-libxml2. Запусти `make`. GitHub Actions зберігає готовий NRO як артефакт.
