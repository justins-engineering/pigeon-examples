# Decoding uploaded device logs

`CONFIG_PIGEON_LOG_UPLOAD` is on in `https_init`, `coap_dtls_init` and `mqtt_init`. NIDD carries
no log upload, so `nidd_init` sends none. A background ring buffer captures the device's own log
output through Zephyr's dictionary-based logging and sends it to the platform in batches,
authenticated the same way as telemetry and shadow reports.

The win is that format strings never ship in the firmware image or over the air. Each record on the
wire is a source id, a level, a timestamp and packed arguments, so reading it back needs a lookup
table that stayed on the build host.

## The dictionary is a per-build artifact

Enabling log upload makes every build emit `log_dictionary.json` beside the application ELF:
`build/zephyr/log_dictionary.json` for a plain build, `build/<sample>/zephyr/log_dictionary.json`
under sysbuild. It is never flashed and never uploaded, and it does not carry over between builds.
A chunk uploaded by one build decodes only with that build's dictionary.

Nothing ties an uploaded chunk back to the dictionary that decodes it, and a mismatched pair
decodes to plausible nonsense rather than erroring. Archive the dictionary alongside whatever image
was flashed.

## Getting a chunk

What the device sends is the raw binary stream its ring buffer held: concatenated dictionary
records, no JSON envelope and no framing of its own. The platform keeps the last 200 chunks per
pigeon and serves them to the owning dashboard user at `GET /pigeons/:id/logs`, oldest first, as
`{id, data, received_at}` objects whose `data` is that chunk base64-encoded for JSON transport.

Decode the base64 back to bytes and save each chunk to its own file untouched. The decoder wants
the bytes the device sent, not the base64 text, and there is nothing else to unwrap.

## Running the decoder

Zephyr's own decoder handles this unmodified:

```sh
source .venv/bin/activate
python3 zephyr/scripts/logging/dictionary/log_parser.py <dictionary> <chunk file>
```

The two positional arguments are the dictionary database and the raw log data. The `--hex` and
`--rawhex` flags are for hex-encoded transports such as a text console, not for this path, which is
raw binary end to end.

Point it at an empty file to check the parser and the dictionary are paired correctly before there
is a real chunk to decode. It exits 0 with no output:

```sh
python3 zephyr/scripts/logging/dictionary/log_parser.py <dictionary> /dev/null
```
