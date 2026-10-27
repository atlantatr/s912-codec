#!/bin/sh
# Gradle wrapper launcher (minimal). Tam versiyon icin:
#   gradle wrapper (Android Studio / lokal) ile uretilir.
# Bu dosya, CI'da "gradle wrapper" task'i cagrilmasi icin tutar.
exec gradle "$@"
