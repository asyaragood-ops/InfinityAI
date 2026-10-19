# InfinityAI
Ниже порядок запуска на Windows. Понадобятся два окна: в одном работает бэкенд, в другом сайт.

1. Установка (один раз)
Node.js версии 22 LTS с nodejs.org.
MSYS2 с msys2.org. После установки откройте терминал MSYS2 UCRT64 и выполните:
bash
  pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
                     mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-sqlite3 mingw-w64-ucrt-x86_64-openssl
2. Бэкенд (терминал MSYS2 UCRT64)
bash
cd /c/Users/Administrator/Desktop/infinityAI/backend
rm -rf build                      # только если папка осталась от старой сборки
cmake -B build -G Ninja
cmake --build build
./build/infinity-router

Бэкенд заработает на http://127.0.0.1:8080, и в логе появится список моделей каскада. Окно не закрывайте.

3. Фронтенд (обычный PowerShell или cmd)
powershell
cd C:\Users\Administrator\Desktop\infinityAI\frontend
npm install        # только при первом запуске
npm start

Затем откройте в браузере http://localhost:4200.

4. Ключи и настройки
Откройте Настройки → API-ключи и введите хотя бы один бесплатный ключ:
Google AI Studio: https://aistudio.google.com/apikey
Groq: https://console.groq.com/keys
Ключ сразу уходит на бэкенд, и модели этого провайдера попадают в каскад. На странице «Модели» у них появится пометка «в каскаде».
По желанию включите «Шифровать ключи на сервере» и нажмите «Сохранить».
Если что-то не работает
В чате пишет «демо-режим» или «бэкенд не запущен». Проверьте, что окно MSYS2 с бэкендом открыто и в нём нет ошибок.
Порт 8080 занят. Поменяйте "port" в backend/config.json, а затем target в frontend/proxy.conf.json.
Бэкенд запускается из Проводника, а не из MSYS2, и не стартует. Не хватает библиотек. Запускайте его из терминала MSYS2 UCRT64.
Ошибка сборки про OpenSSL. Не установлен пакет mingw-w64-ucrt-x86_64-openssl из шага 1.
Ключи перестали расшифровываться. Не удаляйте backend/data/master.key: без него зашифрованные ключи придётся ввести заново.
