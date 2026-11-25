# Remote Suite

Open **THE ZEN → REMOTE SUITE** to use a custom button layout with a remote from your SD card.

## Create a layout

1. Save a profile in **IR DISCOVERY**, or put a Flipper `.ir` file in `/ir/remotes/` on the SD card.
2. Open **REMOTE SUITE → CREATE REMOTE** and choose the Flipper file. Use **Left/Right** to scroll long names in a list.
3. Use **Up/Down** to choose a slot and **A** to assign a command. There are eleven slots: the four D-pad directions, standalone **A** and **B**, the four **A + direction** combinations, and **A + B**. The editor shows the POOM control above its assigned remote command. Select **Unassigned** to clear a slot.
4. Hold **A** for one second from any slot, or select **SAVE LAYOUT**, then give it a name. Each layout is saved under `/ir/suite/`. It contains a copy of the assigned commands, including RAW captures, so you can make multiple layouts from one source and use them independently.

## Use a layout

Choose **OPEN REMOTE** and select a saved layout. Press a direction to send its assigned signal. Hold **A** and press a direction to send a command from the second layer. The playback screen shows the regular buttons and last command sent. Holding **A** switches the screen to the **[FN]** shortcuts. Tap **A** or **B** to send its standalone assignment. Hold **A** and press **B** to use the **A + B** assignment. After either type of shortcut, releasing the buttons sends no additional command. Hold **B** for one second to return to Remote Suite. If B is unassigned, a tap also goes back.

## Edit or delete a layout

- **EDIT REMOTE** opens the current assignments. The assignment list includes the original Flipper file’s commands and the copies already in the layout. Choose **Unassigned** to clear a slot. Hold **A** for one second or select **SAVE LAYOUT** to update the selected layout under the same name. Older layouts ask you to select their original file once. Saving remembers that link. If the original is unavailable, press **B** at the source picker to edit using the saved commands.
- **DELETE REMOTE** shows the selected layout and defaults to **Keep remote**. Select **Delete layout** to remove its copy under `/ir/suite/`. The source Flipper file remains available under `/ir/remotes/`.

## Hardware checks

- Import a saved IR Discovery profile that includes both parsed and RAW commands.
- Assign commands to a plain direction and the same direction with the A modifier. Save the layout, reboot and verify both commands on the TV.
- Check that standalone A and B send their assignments, releasing A after a shortcut sends no extra command, A + B sends only its own assignment, and holding B on its own returns to Remote Suite without sending its assignment.
- Exit Remote Suite with B and verify that POOM returns to **THE ZEN**. Open the original profile through **IR UNIV → FLIPPER FILE** and verify its commands.
