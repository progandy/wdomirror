# wdomirror

wdomirror utilizes the wlroots dmabuf export protocol to create a mirror of an outout
with as little overhead as possible.

## Building

```bash
meson build && ninja -C build
```

## Usage

List the outputs and their IDs.

```bash
./wdomirror
```
Create the mirror

```bash
./wdomirror $ID
```

wdomirror does not preserve the aspect ration, so make sure to set the size of the mirror window correctly.
For fullscreen, make sure that both source and target outputs have the same aspect ratio.


## License

[MIT](LICENSE)
