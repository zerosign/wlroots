libseat = dependency('libseat',
	version: '>=0.2.0',
	fallback: 'seatd',
	default_options: ['server=disabled', 'man-pages=disabled'],
)
wlr_files += files('session.c')
wlr_deps += libseat

# libudev

if get_option('enum-backend') == 'auto'
	udev = dependency('libudev', required: false)
	if udev.found()
		wlr_files += files('dev_udev.c')
		wlr_deps += udev
		features += { 'udev': true }
	endif
elif get_option('enum-backend') == 'udev'
	wlr_files += files('dev_udev.c')
	wlr_deps += dependency('libudev')
	features += { 'udev': true }
endif
