# G940-X-Plane
X-Plane Linux plugins for Logitech Flight System G940

**Linux only**; Windows users could instead look at [XPForce](https://www.fsmissioneditor.com/product/xpforce/)
(but it's not mine and I haven't tried it).

This repo contains two plugins for X-Plane that work with the Logitech G940:
* g940FF: force feedback
  * not specific to G940 but untested with other hardware
  * very much work in progress
* g940LEDs: controls the LEDs on the throttle
  * needs a config UI adding; no other major changes planned

See also my [kernel patches](https://github.com/chrisboyle/G940-linux) for the G940, which are **required** for the
LED plugin and will improve your experience with the force feedback plugin.

## LED mapping
This is not yet configurable, and obviously makes a lot more sense when the button press action and the LED colour
relate to the same item. For now, either edit the source or remap your buttons P1 to P8 in X-Plane as follows:

<table>
<tr><td>speedbrakes<br/>retract one</td><td>flaps<br/>retract one</td>
<td>carb heat<br/>toggle</td><td>autopilot<br/>servos toggle</td></tr>
<tr><td>speedbrakes<br/>extend one</td><td>flaps<br/>extend one</td>
<td>landing light<br/>toggle</td><td>landing gear<br/>toggle</td></tr>
</table>

## Installation
`make install`

This will download the X-Plane SDK, compile the plugins, try to determine your X-Plane installation location and
install both plugins into it.
