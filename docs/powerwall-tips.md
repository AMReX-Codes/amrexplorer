# Using AMReXplorer with a Powerwall

A [powerwall](https://en.wikipedia.org/wiki/Powerwall) is a multi-monitor matrix
display that is designed to be produce room-sized visualization of simulations.

There are several tricks that can improve the user experience when using
AMReXplorer with a powerwall.

## Increasing the size of the GUI elements

The GUI elements and fonts can be rescaled to be human-readable using the
`QT_SCALE_FACTOR` environment variable. This works on Linux, Mac, and WSL,
so it is usually the easiest solution.

It can be set immediately at the command line:
```console
$ QT_SCALE_FACTOR=4 amrexplorer
```

Alternatively, it can be set in the shell configuration file (`~/.bashrc` or `~/.zshrc`):
```console
$ echo 'export QT_SCALE_FACTOR=4' >> ~/.bashrc
```

## Maximizing the window to fit the powerwall

Some powerwalls have control monitors in addition to the monitors on the wall itself.
In this case, you will need to maximize the window to fit the powerwall without spanning
all displays connected to the machine.

The solution is platform-specific. On Windows, common solutions include using the FancyZones
PowerToy to create a Zone that spans only the powerwall display. Then you can maximize the window
to fit the powerwall display by holding `Shift` and dragging the window to the powerwall display.
Another method is to write a custom hotkey script for AutoHotkeys v2. This requires manually
identifying the IDs of the powerwall displays. Unfortunately, these methods only work
reliably for Win32 applications and we have found that neither of these methods work for
WSL applications.

A workaround for AMReXplorer when launched from WSL is in development.

## Increasing the data cache size

AMReXplorer only loads data up to its internal cache size. For large simulations, this will lead
to reduced resolution visualizations. You may want to increase the cache size to as
large of a value as your system RAM on the visualization node can support.

The `AMREXPLORER_CACHE_SIZE_MB` environment variable sets the data cache size in MB.
This can be set when launching on the command line:
```console
$ AMREXPLORER_CACHE_SIZE_MB=8192 amrexplorer
```
