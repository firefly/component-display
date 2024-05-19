Firefly Display
===============

A simple display driver for the ST7789 240x240 display.


Example
-------

See the [example project](./examples/test-app) which merely
displays a static image on the display.


Using Locally
-------------

Due to the way ESP IDF uses a component's folder name to determine
its package name, the default folder when checking out from GitHub
(i.e. `component-display`) prevents the component from working.

To resolve this, for example if contributing to the repository,
checkout out the component in a target folder named `firefly-display`:

```shell
/home/ricmoo> git clone git@github.com:firefly/component-display.git firefly-display
# This is important!! -----------------------------------------------^
```


License
-------

MIT License.
