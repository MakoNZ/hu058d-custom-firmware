## Analysis data export

`tools/export-hu058d-data.sh` creates a timestamped analysis bundle containing
both the long-term data stored by Gladys and the clock's current in-RAM NTP
history.

By default it exports the previous seven days from Gladys and discovers all
features belonging to the `HU058D` device. It asks Gladys for an effectively
unlimited `max_states` value so the API returns every stored state in the
requested window rather than reducing the result to chart-sized buckets.

It also downloads:

- `/api/ntp-history` from the clock, including the current 48-sample ring buffer
- `/api/status` as a point-in-time diagnostic snapshot
- untouched JSON responses
- normalized CSV files
- one CSV per Gladys feature
- a manifest containing the export time, requested window, selectors and Git
  revision
- a `.tar.gz` containing the complete export for convenient analysis/sharing

Example:

```bash
./tools/export-hu058d-data.sh --days 7
```

Other useful windows:

```bash
./tools/export-hu058d-data.sh --hours 12
./tools/export-hu058d-data.sh --days 30
```

Output is written under `analysis-data/<timestamp>/`; `analysis-data/` should be
excluded from Git because it contains generated telemetry rather than source.

The Gladys and clock addresses can be overridden without editing the script:

```bash
GLADYS_URL='http://homepi.lan:8083' \
CLOCK_URL='http://192.168.1.40' \
./tools/export-hu058d-data.sh --days 7
```

If the Gladys API requires authentication, set `GLADYS_TOKEN` in the environment.
Feature selectors can also be supplied explicitly with `--selectors` or the
`GLADYS_FEATURES` environment variable.
