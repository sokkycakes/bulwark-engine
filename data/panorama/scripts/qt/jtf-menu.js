// JTF main menu: tile actions. Its fonts (Iceberg, Kode Mono; SIL OFL, in
// panorama/fonts-jtf/) are registered by panorama/jtf-menu.cfg first.

function JtfMenuAction( what )
{
	// The co-op menu progs (Menu_UIAction) decides what each tile opens.
	Bulwark.Send( 'jtf_menu', what );
}
