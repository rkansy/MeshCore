# T-LoRa Pager chat firmware

This app builds as `env:LilyGo_TLoRaPager_pager` on the Stage 2 T-LoRa Pager variant. It uses `BaseChatMesh` for private and group messaging, the variant's interrupt-driven TCA8418 keyboard and rotary input, and the ST7796 display through `LGFXDisplay`.

For diagnostics, `env:LilyGo_TLoRaPager_pager_debug` routes `Serial` to native USB CDC, disables the screen timeout, and prints startup-stage diagnostics through ESP-IDF logs. Use `/dev/ttyACM*` for this environment; the normal pager environment keeps the CP210x UART console. `env:LilyGo_TLoRaPager_display_test` isolates the ST7796 path: it cycles labeled solid RGB565 fills before radio init, after radio init without touching LoRa CS, after radio init with LoRa CS forced high, and after a post-radio display reinit. `env:LilyGo_TLoRaPager_display_radio_first` mirrors Meshtastic's bus order by initializing the radio on FSPI/SPI2 before bringing up the ST7796, then cycling labeled fills. `env:LilyGo_TLoRaPager_display_mesh_test` also starts `PagerMesh` (including its initial advert) before panel init, then cycles fills while the mesh loop runs. These diagnostics separate panel/power issues from shared-SPI/CS and init-order issues.

## Persistent state

SPIFFS is used on the board's 16 MB flash. The companion `DataStore` persists the identity, node preferences, contacts, and channels; `/pager.cfg` stores the pager-only screen timeout, brightness, keyboard-light preference, and selected channel. The inbox and active draft are intentionally RAM-only. Private identity and channel PSKs remain in the local filesystem and are never printed by this application.

## Controls

- Rotary: move in lists; move the caret while composing.
- Rotary press (GPIO 7): select, commit, or send.
- Side button (GPIO 0): back; saves the draft when leaving compose.
- Keyboard: text entry; DEL removes the character before the caret; ENTER sends/selects; keyboard digits 1–5 switch to Inbox/Compose/Nodes/Channels/Settings when not in a text field.
- Channels screen: side button opens Add Channel; enter name and up to 16 hexadecimal PSK characters, then press the encoder to join/select it.

Radio settings are saved immediately and applied at the next radio initialization (reboot). Display-off leaves the radio active. Battery and keyboard-light behavior follow the screen timeout setting.
