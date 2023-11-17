# trackfile.device and its buddy DAControl

AmigaOS 3.2 introduced the trackfile.device and its control command
named "DAControl" as the means to make installing the operating system
from CD-ROM, floppy disk or any other medium easier.

Early on we had decided to use the same installation script, regardless
of what medium to install from and the lowest common denominator to
enable this was to use "virtual floppy disks". These would have to be
inserted/removed as needed by the installation script and the virtual
floppy disk drive should be under script control.

That virtual floppy disk drive is "trackfile.device" and the associated
control command is "DAControl". Their source code, to be compiled and
built using SAS/C 6, is presented here in the hope that it will be useful
and could serve as building blocks for other software which might have
the same needs as AmigaOS 3.2.

The "trackfile.device" source code in particular attempts to show how
to make an AmigaOS device driver work which emulates the behaviour and
design of the original "trackdisk.device" as closely as possible.
Complex device drivers which fit an existing specification and are
documented to a degree which might make your head spin are rare and
maybe that is a good thing. Careful, you could learn something from what
is presented here. Small as it may seem, the "trackfile.device" cuts
a swath through the AmigaOS stack.

As a bonus, source code for associated technology is provided, in this
case raw disk read/write & recovery tools and a CLI command which
calculates checksums for ADF disk images in the same way which both
"DAControl" and "trackfile.device" do.

Have fun! Build something new and more powerful from these blocks!

(Bug fixes and enhancements welcome)
