# CalliforniaUpdaterApp

Апдейтер десктоп-клиента Callifornia: маленький нативный exe, который применяет
скачанное клиентом обновление — дожидается выхода приложения, атомарно
подменяет файлы и перезапускает клиент.

Ключевые свойства:

- **Ноль зависимостей на рантайме**: без Qt и OpenSSL, статический CRT
  (`/MT`) — апдейтер подменяет DLL рядом с собой и не должен зависеть от них.
  SHA-256 — Windows CNG (bcrypt), Ed25519 — вендоренный orlp/ed25519
  (только verify), JSON — вендоренный nlohmann single-header.
- **Подписанные манифесты**: перед любыми действиями проверяется Ed25519-подпись
  `manifest.json` вшитым публичным ключом (`src/public_key.hpp`, генерируется
  скриптом `CalliforniaApp/ci/release/generate-update-keys.mjs`).
- **Атомарный swap с журналом**: staged-файлы копируются в
  `<install>\.update\incoming\` (после этого swap — только same-volume rename,
  атомарно на NTFS); журнал `.update\journal.json` + RunOnce-страховка
  позволяют докатить обновление после сбоя/перезагрузки (`repair`).
- **Откат**: если swap упал (залоченный файл и т.п.) — восстановление из
  `.update\backup\` и перезапуск старой версии. Если и `repair` не может
  докатить — он тоже откатывается на предыдущую версию и запускает её, вместо
  того чтобы оставить журнал, из-за которого клиент перестаёт стартовать вовсе.
- **Не зависает никогда**: на весь запуск заведён бюджет (10 минут). Когда он
  кончается, апдейтер записывает, на каком шаге стоял, поднимает приложение и
  убивает сам себя (`TerminateProcess`, exit 9) — гарантия не зависит от того,
  предусмотрен ли конкретный сбой. Ретраи файловых операций тоже тратят этот
  бюджет и оставляют резерв на откат.
- **Ожидание разблокировки всех файлов**: перед swap каждый затрагиваемый файл
  получает до 30 секунд на то, чтобы его отпустили, а не только `Callifornia.exe`.
  Кто именно держит файл, спрашивается у Restart Manager и пишется в лог.
- WIN32-subsystem (никаких мелькающих консолей); лог в
  `%LOCALAPPDATA%\Callifornia\logs\updater.log` — каждый шаг отмечен строкой
  `phase:`; результат — в `%LOCALAPPDATA%\Callifornia\updates\last-apply.json`
  (`status`, `version`, `phase`, `detail`), клиент показывает это пользователю.

## CLI

```
updater.exe apply  --install-dir <dir> --staging-dir <dir> --parent-pid <pid>
                   --relaunch-exe Callifornia.exe [--no-relaunch] [--log <file>]
updater.exe repair --install-dir <dir> [--relaunch-exe Callifornia.exe] [--log <file>]
```

Клиент перед запуском копирует `updater.exe` из install-папки в
`%LOCALAPPDATA%\Callifornia\updates\bin\` (нельзя запускать из папки, которую
предстоит обновлять) и штатно завершается; апдейтер ждёт выхода по PID и
эксклюзивной пробе `Callifornia.exe`.

Exit-коды: `0` применено; `2` аргументы/уже работает другой апдейтер; `3`
приложение не вышло; `4` подпись/хэш не сошлись; `5` нет прав записи; `6` swap
упал, откат успешен; `7` swap и откат упали (журнал оставлен — поможет
`repair`); `8` repair: делать нечего; `9` бюджет запуска исчерпан, процесс убит
сторожевым таймером.

Пока апдейтер работает, он держит named mutex `Local\CalliforniaUpdaterApplying` —
клиент проверяет его при старте и молча выходит, если идёт обновление.

## Сборка и тесты

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
build\Release\updater_tests.exe
powershell -File scripts\integration-test.ps1
```

Интеграционному тесту нужен приватный ключ, парный вшитому в
`src/public_key.hpp`. Такой ключ ровно один, и он лежит в сейфе:
`C:\projects\.callifornia-secrets\updates-manifest-private.pem`. Скрипт берёт
его по умолчанию, отдельная сборка дерева с подменённым ключом не нужна.

Ключ в `CalliforniaBackend/Updates/keys/manifest-private.pem` вшитому больше не
парен. Пока скрипт указывал на него, тест останавливался на первом же apply с
кодом 4 и строкой `manifest signature verification failed`, а это ровно то, как
выглядел бы по-настоящему сломанный апдейтер.

Последний сценарий теста держит файл открытым дольше, чем переживал бы один
только backoff переименования, и проверяет, что обновление это пережидает.
Именно этого ожидания раньше не было, и на нём терялись обновления, которые
перепрыгивают через релиз.

## Связанные части системы

- **CalliforniaApp** — качает обновления (per-file diff по манифесту) в
  `%LOCALAPPDATA%\Callifornia\updates\staging\<version>\` и запускает апдейтер.
- **CalliforniaBackend/Updates** — сервис, отдающий манифесты; блобы файлов клиент
  качает с `/v1/updates/files/<sha256>` (MinIO через Nginx).
- **CalliforniaApp/ci/release/publish-release.mjs** — публикация релиза (подпись
  манифеста, залив блобов); `updater.exe` входит в состав каждого релиза.
  Полный процесс выкатки — [CalliforniaApp/RELEASING.md](https://github.com/frontmany/CalliforniaApp/blob/main/RELEASING.md).
