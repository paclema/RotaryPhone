# Findings

## Ring Bell control
Using the MX1616 H-bridge I can only get 10v output max and it doesn't seem sufficient to ring the 1900 ohm coil bell.
Trying with a stepper motor driver which also acts as an H-bridge and can handle up to 45v and 3A.

My bell already works with 20V supply. I have used a boost converter to provide the high voltage from the USB 5v supply.

I used first a DRV8825 which is not working well, using an A4988 instead and it worked! The issue was that VDD was not present on the DRV8825 module or it was just a malfunctioning module but using VDD = 3V3 enables the driver.

Pinout instructions:
- VMOT: 30 V (depending on the coil). Add nearby capacitor (≥100 µF + 100 nF) between VMOT and GND (power) is recommended.
- GND (power) → GND of the supply and common with ESP32 GND.
- VDD → 3V3 of the ESP32. GND (logic) → GND of the ESP32.
- STEP → GPIO19 of the ESP32. Use PullDown resistor (10k) to avoid false steps when ESP32 boots, ringing the bell unexpectedly.
- DIR  → GPIO18 of the ESP32.
- ENBL → to GND (enabled) [or to a GPIO if you want to control it].
- SLP  → to 3V3 (active).
- RST  → to 3V3 (active). If your board connects SLP and RST, connect them together to 3V3.
- M0, M1, M2 → to GND (full step); don't leave them floating.
- Bell coil → A1 and A2 (B1/B2 unused).
- Adjust driver Vref potentiometer to your coil's nominal current.


## Phone handset detection
Line between C and 4 detects if the handset is on-hook or off-hook.

## Audio interface

[Working with I2S](https://youtu.be/m-MPBjScNRk?si=G3xU8NdSm8ess1sW)
[Audio input I2S and DMA](https://youtu.be/3g7l5bm7fZ8?si=P7WPj4WDnWshptOw)
[ESP32 Audio Input Using I2S and Internal ADC](https://youtu.be/pPh3_ciEmzs?si=hmHEdxSMAkmdApQy)

[ESP32_ADC_Calibration_tool](https://github.com/tommag/ESP32_ADC_Calibration_tool)

Other perhaps useful libs:
- [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools
- [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP)
- [ESP32 audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S)

### Audio input


### Audio output
