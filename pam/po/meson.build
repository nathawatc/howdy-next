i18n = import('i18n')

gettext_package_cpp_arg = '-DGETTEXT_PACKAGE="@0@"'.format(meson.project_name())
localedir_cpp_arg = '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir'))
add_project_arguments(gettext_package_cpp_arg, localedir_cpp_arg, language: 'cpp')

i18n.gettext(
	meson.project_name(),
	args: [
		'--directory=' + meson.current_source_dir(),
		'--keyword=S:1',
	],
)
