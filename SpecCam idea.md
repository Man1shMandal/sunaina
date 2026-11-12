# SpecCam: plug and play quality inspection

## The problem

If a factory wants a camera to check product quality, someone has to build a custom vision model for each product. That means collecting images, labelling defects, training and testing. It takes weeks and most small factories can't afford it, so they still check by eye.

## The idea

Connect a camera to a small board. Tell the system in plain language what the product is and what counts as a defect, and show it a few good and bad samples. It sets up the inspection on its own, without an ML team.

## How it works

1. Setup chat: Muse Spark asks about the product and defects, asks follow-up questions when something is unclear (for example "is a 1mm scratch a reject?"), and writes an inspection spec.
2. Regions: SAM 3.1 finds the product and the parts that matter in each frame.
3. Examples: Muse Image makes more defect examples from the 3 to 5 real ones.
4. Live checks: a small open-weights Meta model runs locally next to the line, so it's fast and cheap. Only unsure parts go back to Muse Spark.
5. Self-test: before going live, it tests itself on held-out samples and shows how accurate it is.

## Hardware

ESP32-CAM, USB camera or phone as the camera. Raspberry Pi or laptop runs the model.

## 10 day plan

- Days 1 to 3: setup chat and spec format
- Days 4 to 5: SAM 3.1 region finding
- Days 6 to 7: extra examples and the local model
- Days 8 to 9: live camera loop, dashboard and self-test
- Day 10: demo and results

## Demo

Set up two different products (for example bottle caps and printed labels) in under 10 minutes each just by talking to it, then run live checks on camera. Report setup time, precision and recall.

## Related past work

Critical Fair (github.com/pratyushadk/criticalfair), which automates AS9102 inspection reports from technical drawings.
