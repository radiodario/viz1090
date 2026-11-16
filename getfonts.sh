#!/bin/bash
# Download the London Underground dot-matrix fonts used by the departures board.
# They are not redistributed here: github.com/petykowski/London-Underground-Dot-Matrix-Typeface
set -e
cd "$(dirname "$0")/font"
BASE=https://raw.githubusercontent.com/petykowski/London-Underground-Dot-Matrix-Typeface/master/font
for weight in Regular Heavy Bold; do
  curl -fsSL -o "LondonUnderground$weight.ttf" "$BASE/London%20Underground%20$weight.ttf"
  echo "font/LondonUnderground$weight.ttf"
done
