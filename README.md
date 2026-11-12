# Project Sunaina

Self-configuring visual inspection on low-cost ESP32 hardware.

*Sunaina* means "the one with good eyes".

## About

This is my major project for my B.E. in Electronics and Communication Engineering at BMS College of Engineering, Bengaluru.

Factories use cameras to check products for defects. Every time a new product comes on the line, the camera system has to be set up again by a vision engineer, which takes time, skill and money. Many small factories can't afford it and still check by eye.

Project Sunaina tries to make that setup simple. You show the system the new product, and it sets up the inspection by itself.

## Hardware

| Part | Role |
|---|---|
| ESP32-CAM | Camera |
| ESP32-S3 | Runs the inspection |
| MG995 servo | Moves the camera |
| OLED display | Shows PASS / FAIL |

## Status

Work in progress.

- [x] Board tests (ESP32-CAM, ESP32-S3)
- [ ] Servo test
- [ ] Camera to S3 link
- [ ] Inspection on the S3
- [ ] Automatic setup for new products
- [ ] Demo

## Author

Manish Kumar Mandal
