# PowerJeep. A Ryobi Battery Conversion for Ride on Cars

[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

> Upgrade your electric ride on car with a 18v Ryobi battery and ESP32-powered electronics.

<div align="center">
   <img src="./docs/web-interface.jpeg" alt="screenshot of the web interface" width="300px"/>
</div>

## Table of Contents

- [Introduction](#introduction)
- [Features](#features)
- [Requirements](#requirements)
- [Assembly](#assembly)
- [Usage](#usage)
- [Contributing](#contributing)
- [License](#license)

## Introduction

Welcome to the Ryobi Battery Conversion project for ride on cars!

This project aims to replace the old lead battery in your electric ride on car with a modern 18v Ryobi battery, providing improved performance!

By utilizing an ESP32 microcontroller, we enable enhanced control and monitoring capabilities for a safe and more enjoyable driving experience for every rider.

## Features

- **Ryobi Battery Integration:** Upgrade your kid car's power source to a Ryobi battery for extended runtime and enhanced performance.
- **ESP32 Control:** Utilize the ESP32 microcontroller for precise control, monitoring, and remote safety features.
- **Real-time Data Display:** Monitor the speed, configure and update the car through a sleek web-based dashboard.
- **Battery Protection:** Use safeguards to prevent over use and deep discharging.
- **Easy Customization:** Adapt the project to fit your specific kid car model and requirements.


⚠️ This project can send up to 18V to your motor, when they are usually taking 12V! It ramps up power to avoids gearbox damage, but be aware it can potentially fry your motor. Make sure it has the proper airflow to keep it cool

## Requirements

To replicate this project, you'll need the following things:

- Ride on car with a functionnal motor
- 18v Ryobi battery and charger
- 3D printer
- Basic hand tools (screwdriver, wire cutter/stripper, soldering iron)

### Electronic components

- 1x ESP32 [On Amazon](https://www.amazon.com/ESP-WROOM-32-Development-Microcontroller-Integrated-Compatible/dp/B08D5ZD528/ref=sr_1_4?keywords=esp32&qid=1686115575&sr=8-4)
- 1x or 2x LM2596 - Voltage Regulator [On Amazon](https://www.amazon.com/gp/product/B08BLBYWN1/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&psc=1)
- 1x XH-M609 - DC 12V-36V Voltage Protection Module [On Amazon](https://www.amazon.com/gp/product/B08X3HZ69D/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&psc=1)
- 1x BTS7960 43A High Power Motor Driver Module [On Amazon](https://www.amazon.com/gp/product/B07TFB22H5/ref=ppx_yo_dt_b_asin_title_o09_s00?ie=UTF8&psc=1)
- 1x 30A Circuit Breaker [On Amazon](https://www.amazon.com/gp/product/B096ZTV3CR/ref=ppx_yo_dt_b_asin_title_o09_s00?ie=UTF8&psc=1)
- Wires and connectors

## Assembly

### Hardware

- Print the battery mount (I used this [one](https://www.thingiverse.com/thing:4587319))
- Print the component support [Source available on OnShare](https://cad.onshape.com/documents/73e5cd159b60a9bf46e87dae/w/8d4b4ae9f68daee1281f112d/e/6d32cd17a65725a466bf965e?renderMode=0&uiState=64801221829a90766f018f83)

### Software

#### First installation

1. Begin by cloning this repository to your local machine.
2. Open the project in VS Code with PlatformIO installed
3. Connect your ESP32 to your computer
4. Build & Upload the project
5. Upload Filesystem

### Update

1. Open the project in VS Code with PlatformIO installed
2. Build
3. Turn on the ride on car & Connect to the wifi
4. Drag and drop firmware.bin from `.pio` folder (or any static resources) onto the upload icon of the webpage

## Usage
- Turn on the fuse and drive!
- Don't forget to turn the fuse off when you are done.
- To go further
  - Connect your computer or mobile device to the Wi-Fi network emitted by the car.
  - It should open the page automatically as a captive portal. If it doesn't, open a web browser and enter the IP address http://192.168.4.1 to access the dashboard.
  - Use the interface to configure the car and view real-time speed. Emergency stop turns off the motor immediately.

## Contributing
Contributions are welcome! 

If you have any ideas, improvements, or bug fixes, please submit a pull request. For major changes, please open an issue first to discuss potential updates.

## License
This project is licensed under the MIT License.
