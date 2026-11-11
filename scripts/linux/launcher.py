#!/usr/bin/env python3
"""The OpenSmash Ultimate launcher: choose your own game files, build the native Linux game, and play it.

    python scripts/linux/launcher.py
    python scripts/linux/launcher.py --check-tools     check clang / pkg-config / the Python packages, no window
    python scripts/linux/launcher.py --self-test       open the window and close it (for CI under Xvfb)

The window is a Fedora Media Writer-style wizard: one decision per page. It starts
scripts/linux/install_linux.py for the dump and the build, then scripts/run.py --linux --play for the game. Everything
writable lives in one storage folder (default: $XDG_DATA_HOME/ssbu-recomp, in the Flatpak
~/.var/app/io.github.Rsoto95.OpenSmashUltimate/data/ssbu-recomp).
"""
import argparse
import json
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SETUP_STEPS = ['deps', 'stage', 'convert', 'modules', 'tables', 'lift', 'sdk', 'compile', 'nro', 'link', 'check']
SETUP_W = {'deps': 1, 'stage': 1, 'convert': 1, 'modules': 2, 'tables': 1, 'lift': 15, 'sdk': 8,
           'compile': 55, 'nro': 12, 'link': 3, 'check': 1}
SETUP_TOTAL = sum(SETUP_W.values())
DUMP_BAR = 0.20
PAGE_TITLES = [('Select your game files',
                'Super Smash Bros. Ultimate is built on this PC from your own dump. Nothing of the game is downloaded.'),
               ('Choose the game files',
                'Pick the base game and the 13.0.4 or 13.0.5 update, and the prod.keys that decrypt them.'),
               ('Extras',
                'Optional: DLC packages and your console firmware, for its own fonts. You can add them later.'),
               ('Install location',
                'Where the dump, the build and the saves go. About 30 GB is used by the first install.'),
               ('Review',
                'Check the choices, then press Install. The first build takes 1.5-2.5 hours and can be stopped.')]
PAGES = ['source', 'files', 'extras', 'location', 'review']


def default_storage():
    return os.environ.get('SSBU_DATA_DIR') or os.path.join(
        os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'), 'ssbu-recomp')


def config_path():
    return os.path.join(os.environ.get('XDG_CONFIG_HOME') or os.path.expanduser('~/.config'),
                        'ssbu-recomp', 'launcher.json')


def read_json(path):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def free_gb(path):
    p = os.path.abspath(path)
    while p and not os.path.exists(p):
        up = os.path.dirname(p)
        if up == p:
            break
        p = up
    try:
        return shutil.disk_usage(p).free / 2.0 ** 30
    except OSError:
        return None


CSS = b"""
.wizard-title { font-size: 20px; font-weight: 700; }
.wizard-subtitle { opacity: 0.66; }
.step-label { opacity: 0.55; font-size: 11px; }
.card { background-color: @theme_base_color; border-radius: 12px; border: 1px solid @borders; }
.card row { padding: 9px 12px; }
.row-title { font-weight: 600; }
.row-subtitle { opacity: 0.66; font-size: 11px; }
.log-view { font-family: monospace; font-size: 11px; background-color: @theme_base_color; }
.next { border-radius: 8px; padding: 6px 16px; }
.action-bar { padding: 6px 10px; }
.page { padding: 16px 18px; }
.hero { margin: 2px; }
"""


def apply_theme(Gtk, GLib, Gio, Gdk):
    """Follow the system dark/light choice and use adw-gtk3 (the libadwaita-like GTK3 theme) when it is installed."""
    dark = False
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        proxy = Gio.DBusProxy.new_sync(bus, Gio.DBusProxyFlags.NONE, None,
                                       'org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop',
                                       'org.freedesktop.portal.Settings', None)
        value = proxy.call_sync('Read', GLib.Variant('(ss)', ('org.freedesktop.appearance', 'color-scheme')),
                                Gio.DBusCallFlags.NONE, 1000, None).unpack()[0]
        dark = (value == 1)
    except Exception:
        pass
    settings = Gtk.Settings.get_default()
    if settings:
        settings.set_property('gtk-theme-name', 'adw-gtk3-dark' if dark else 'adw-gtk3')
        settings.set_property('gtk-application-prefer-dark-theme', dark)
    provider = Gtk.CssProvider()
    provider.load_from_data(CSS)
    screen = Gdk.Screen.get_default()
    if screen:
        Gtk.StyleContext.add_provider_for_screen(screen, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    return dark


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check-tools', action='store_true', help='check the tools, no window')
    ap.add_argument('--self-test', action='store_true', help='open the window and close it at once (CI)')
    ap.add_argument('--storage', help='the storage folder (default: %s)' % default_storage())
    a = ap.parse_args(argv)

    if a.check_tools:
        cmd = [sys.executable, os.path.join(REPO, 'scripts', 'linux', 'install_linux.py'), '--check-tools',
               '--storage', a.storage or default_storage()]
        return subprocess.call(cmd, cwd=REPO)

    import gi
    gi.require_version('Gtk', '3.0')
    gi.require_version('GdkPixbuf', '2.0')
    from gi.repository import Gtk, GLib, Gdk, Gio, Pango, GdkPixbuf
    apply_theme(Gtk, GLib, Gio, Gdk)
    App = Launcher(Gtk, GLib, Gdk, Gio, Pango, GdkPixbuf, a.storage)
    if a.self_test:
        App.win.show_all()

        def cycle():
            for name in ('source', 'files', 'extras', 'location', 'review', 'progress', 'ready'):
                App._show_step(name)
            GLib.timeout_add(300, Gtk.main_quit)
            return False

        GLib.timeout_add(300, cycle)
        Gtk.main()
        print('self-test ok', flush=True)
        return 0
    App.win.show_all()
    Gtk.main()
    App.save_config()
    return 0


class Launcher:
    def __init__(self, Gtk, GLib, Gdk, Gio, Pango, GdkPixbuf, storage=None):
        self.Gtk, self.GLib, self.Gdk, self.Gio, self.Pango = Gtk, GLib, Gdk, Gio, Pango
        self.GdkPixbuf = GdkPixbuf
        self.cfg = read_json(config_path()) or {}
        self.storage = os.path.abspath(storage or self.cfg.get('storage') or default_storage())
        self.mode = self.cfg.get('mode', 'packages')
        self.dlc = [p for p in self.cfg.get('dlc', []) if isinstance(p, str)]
        self.proc = None
        self.game_proc = None
        self.stopping = False
        self.q = queue.Queue()
        self.page_name = 'source'
        self.step = 0
        self.setup_state = {}
        self.info = read_json(os.path.join(self.storage, 'install.json'))
        self._dialog = None
        self._build_window()
        self._load()
        self._sync()
        self._show_step('source')
        self._refresh_storage()
        self._refresh_review()
        self._refresh_install()
        self.GLib.timeout_add(100, self._pump)

    # -------------------------------------------------------------------------------------------------- window
    def _build_window(self):
        Gtk = self.Gtk
        self.win = Gtk.Window(title='OpenSmash Ultimate')
        self.win.set_default_size(780, 700)
        self.win.set_default_icon_name('io.github.Rsoto95.OpenSmashUltimate')
        self.win.connect('delete-event', self._on_close)

        header = Gtk.HeaderBar()
        header.set_show_close_button(True)
        header.set_title('OpenSmash Ultimate')
        header.set_subtitle('Super Smash Bros. Ultimate, built on this PC from your own dump')
        menu = Gtk.MenuButton()
        menu.set_image(Gtk.Image.new_from_icon_name('open-menu-symbolic', Gtk.IconSize.BUTTON))
        menu.set_tooltip_text('Menu')
        pop = Gtk.Popover()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        box.set_margin_start(6)
        box.set_margin_end(6)
        box.set_margin_top(6)
        box.set_margin_bottom(6)
        for text, cb in (('Open storage folder', self._open_storage),
                         ('Open install log', self._open_log),
                         ('About OpenSmash Ultimate', self._about)):
            b = Gtk.Button(label=text)
            b.set_relief(Gtk.ReliefStyle.NONE)
            b.connect('clicked', lambda _b, f=cb: (pop.popdown(), f()))
            box.pack_start(b, False, False, 0)
        pop.add(box)
        menu.set_popover(pop)
        header.pack_end(menu)
        self.win.set_titlebar(header)

        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.win.add(outer)
        self.stack = Gtk.Stack()
        self.stack.set_transition_type(Gtk.StackTransitionType.SLIDE_LEFT_RIGHT)
        self.stack.set_transition_duration(180)
        self.stack.set_hexpand(True)
        self.stack.set_vexpand(True)
        outer.pack_start(self.stack, True, True, 0)
        self._build_action_bar(outer)
        self._build_source_page()
        self._build_files_page()
        self._build_extras_page()
        self._build_location_page()
        self._build_review_page()
        self._build_progress_page()
        self._build_ready_page()

    def _page(self, title, subtitle):
        Gtk = self.Gtk
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        page.get_style_context().add_class('page')
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        clamp = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        clamp.set_halign(Gtk.Align.CENTER)
        clamp.set_valign(Gtk.Align.START)
        clamp.set_size_request(580, -1)
        # The icon file is 512 px; set_pixel_size() does not scale a file-backed Gtk.Image, so scale the pixbuf.
        try:
            pixbuf = self.GdkPixbuf.Pixbuf.new_from_file_at_scale(
                os.path.join(REPO, 'media', 'opensmash_icon_512.png'), 96, 96, True)
            hero = Gtk.Image.new_from_pixbuf(pixbuf)
        except Exception:
            hero = Gtk.Image.new_from_file(os.path.join(REPO, 'media', 'opensmash_icon_512.png'))
            hero.set_pixel_size(96)
        hero.set_halign(Gtk.Align.CENTER)
        hero.get_style_context().add_class('hero')
        clamp.pack_start(hero, False, False, 0)
        t = Gtk.Label()
        t.set_markup('<span size="x-large" weight="bold">%s</span>' % self.GLib.markup_escape_text(title))
        t.set_halign(Gtk.Align.CENTER)
        t.set_justify(Gtk.Justification.CENTER)
        t.set_line_wrap(True)
        t.get_style_context().add_class('wizard-title')
        clamp.pack_start(t, False, False, 0)
        s = Gtk.Label(label=subtitle)
        s.set_halign(Gtk.Align.CENTER)
        s.set_justify(Gtk.Justification.CENTER)
        s.set_line_wrap(True)
        s.set_max_width_chars(74)
        s.get_style_context().add_class('wizard-subtitle')
        clamp.pack_start(s, False, False, 0)
        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        content.set_margin_top(8)
        clamp.pack_start(content, False, False, 0)
        scroll.add(clamp)
        page.pack_start(scroll, True, True, 0)
        return page, content

    def _card(self):
        lb = self.Gtk.ListBox()
        lb.set_selection_mode(self.Gtk.SelectionMode.NONE)
        lb.get_style_context().add_class('card')
        return lb

    def _row(self, title, subtitle='', suffix=None):
        Gtk = self.Gtk
        row = Gtk.ListBoxRow()
        row.set_selectable(False)
        row.set_activatable(False)
        box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        text = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        t = Gtk.Label(label=title, xalign=0)
        t.get_style_context().add_class('row-title')
        s = Gtk.Label(label=subtitle, xalign=0)
        s.get_style_context().add_class('row-subtitle')
        s.set_line_wrap(True)
        text.pack_start(t, False, False, 0)
        text.pack_start(s, False, False, 0)
        box.pack_start(text, True, True, 0)
        if suffix:
            box.pack_end(suffix, False, False, 0)
        row.add(box)
        return row, s

    def _file_row(self, title, tip, kind, entry):
        entry.set_visible(False)
        btn = self.Gtk.Button(label='Browse...')
        btn.connect('clicked', self._browse, entry, kind, tip)
        row, sub = self._row(title, 'Not chosen', btn)
        sub.set_ellipsize(self.Pango.EllipsizeMode.MIDDLE)
        entry.connect('changed', lambda *_: (self._file_sub(entry, sub), self._update_nav(), self._refresh_review()))
        return row, sub

    def _build_source_page(self):
        Gtk = self.Gtk
        page, content = self._page(*PAGE_TITLES[0])
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        self.rb_packages = Gtk.RadioButton.new_with_label_from_widget(
            None, 'Package files (.nsz / .nsp / .xci) and my keys')
        self.rb_packages.set_active(self.mode == 'packages')
        self.rb_packages.connect('toggled', self._on_mode)
        self.rb_dump = Gtk.RadioButton.new_with_label_from_widget(
            self.rb_packages, 'A dump folder I already made (exefs/ and romfs/ inside)')
        self.rb_dump.set_active(self.mode == 'dump')
        self.rb_dump.connect('toggled', self._on_mode)
        box.pack_start(self.rb_packages, False, False, 0)
        box.pack_start(self.rb_dump, False, False, 0)
        content.pack_start(box, False, False, 0)
        self.play_now_btn = Gtk.Button(label='Play the installed build')
        self.play_now_btn.get_style_context().add_class('suggested-action')
        self.play_now_btn.connect('clicked', self._play)
        self.play_now_btn.set_no_show_all(True)
        self.play_now_btn.set_halign(Gtk.Align.CENTER)
        content.pack_start(self.play_now_btn, False, False, 8)
        self.stack.add_named(page, 'source')

    def _build_files_page(self):
        page, content = self._page(*PAGE_TITLES[1])
        self.base = self.Gtk.Entry()
        self.update = self.Gtk.Entry()
        self.keys = self.Gtk.Entry()
        self.dump = self.Gtk.Entry()
        self.files_list = self._card()
        self.base_row, self.base_sub = self._file_row('Base game', 'The base game (v0)', 'pkg', self.base)
        self.update_row, self.update_sub = self._file_row('Update 13.0.4 or 13.0.5', 'The update package', 'pkg', self.update)
        self.keys_row, self.keys_sub = self._file_row('prod.keys', 'Your console keys (package files only)', 'keys', self.keys)
        self.dump_row, self.dump_sub = self._file_row('Dump folder', 'The folder with exefs/ and romfs/', 'dir', self.dump)
        for row in (self.base_row, self.update_row, self.keys_row, self.dump_row):
            row.set_no_show_all(True)
            self.files_list.add(row)
        content.pack_start(self.files_list, False, False, 0)
        self.files_help = self.Gtk.Label()
        self.files_help.set_xalign(0)
        self.files_help.set_line_wrap(True)
        self.files_help.get_style_context().add_class('wizard-subtitle')
        content.pack_start(self.files_help, False, False, 0)
        self.stack.add_named(page, 'files')

    def _build_extras_page(self):
        Gtk = self.Gtk
        page, content = self._page(*PAGE_TITLES[2])
        content.pack_start(Gtk.Label(label='DLC packages', xalign=0), False, False, 0)
        self.dlc_list = self._card()
        content.pack_start(self.dlc_list, False, False, 0)
        box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        add = Gtk.Button(label='Add DLC...')
        add.connect('clicked', self._add_dlc)
        clear = Gtk.Button(label='Clear')
        clear.connect('clicked', self._clear_dlc)
        box.pack_start(add, False, False, 0)
        box.pack_start(clear, False, False, 0)
        content.pack_start(box, False, False, 0)
        self.fw = Gtk.Entry()
        self.fw_row, self.fw_sub = self._file_row('Firmware (optional)', 'Your firmware folder, for its own fonts', 'dir', self.fw)
        fw_list = self._card()
        fw_list.add(self.fw_row)
        content.pack_start(fw_list, False, False, 8)
        self.stack.add_named(page, 'extras')

    def _build_location_page(self):
        Gtk = self.Gtk
        page, content = self._page(*PAGE_TITLES[3])
        self.location_list = self._card()
        self.storage_entry = Gtk.Entry()
        self.storage_entry.set_visible(False)
        self.storage_entry.set_text(self.storage)
        self.storage_entry.connect('changed', self._on_storage_text)
        b = Gtk.Button(label='Browse...')
        b.connect('clicked', self._browse, self.storage_entry, 'dir', 'Where dump/, build-linux/ and saves go')
        storage_row, self.storage_sub = self._row('Storage folder', self.storage, b)
        self.storage_sub.set_ellipsize(self.Pango.EllipsizeMode.MIDDLE)
        self.location_list.add(storage_row)
        self.jobs = Gtk.SpinButton.new_with_range(1, 64, 1)
        self.jobs.set_value(int(self.cfg.get('jobs', min(24, os.cpu_count() or 8))))
        self.jobs.set_tooltip_text('fewer = the PC stays usable while it builds')
        jobs_row, self.jobs_sub = self._row('Compiler jobs', 'Fewer keeps the PC responsive; the build takes longer', self.jobs)
        self.location_list.add(jobs_row)
        content.pack_start(self.location_list, False, False, 0)
        self.disk = Gtk.Label()
        self.disk.set_xalign(0)
        self.disk.set_line_wrap(True)
        self.disk.get_style_context().add_class('wizard-subtitle')
        content.pack_start(self.disk, False, False, 0)
        self.stack.add_named(page, 'location')

    def _build_review_page(self):
        page, content = self._page(*PAGE_TITLES[4])
        self.review_list = self._card()
        self.review = {}
        for key, title in (('mode', 'Mode'), ('base', 'Base game'), ('update', 'Update'), ('keys', 'prod.keys'),
                           ('dlc', 'DLC packages'), ('storage', 'Storage folder'), ('jobs', 'Compiler jobs')):
            row, sub = self._row(title, '')
            sub.set_ellipsize(self.Pango.EllipsizeMode.MIDDLE)
            self.review[key] = sub
            self.review_list.add(row)
        content.pack_start(self.review_list, False, False, 0)
        note = self.Gtk.Label(label='The first build takes 1.5-2.5 hours and can be stopped and continued. '
                                    'Saves, dump and build stay in the storage folder.')
        note.set_xalign(0)
        note.set_line_wrap(True)
        note.get_style_context().add_class('wizard-subtitle')
        content.pack_start(note, False, False, 0)
        self.stack.add_named(page, 'review')

    def _build_progress_page(self):
        Gtk = self.Gtk
        page, content = self._page('Building the game',
                                   'The first build takes 1.5-2.5 hours. The log below shows every step, including '
                                   'each compiler progress line, so a slow file does not look like a freeze.')
        marks = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=16)
        self.progress_marks = []
        for name in ('Game files', 'Build', 'Ready'):
            l = Gtk.Label(label='○ ' + name)
            l.set_xalign(0)
            marks.pack_start(l, False, False, 0)
            self.progress_marks.append(l)
        content.pack_start(marks, False, False, 0)
        line = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.spinner = Gtk.Spinner()
        line.pack_start(self.spinner, False, False, 0)
        self.status = Gtk.Label(label='Preparing...')
        self.status.set_xalign(0)
        self.status.set_line_wrap(True)
        line.pack_start(self.status, True, True, 0)
        content.pack_start(line, False, False, 0)
        self.bar = Gtk.ProgressBar()
        self.bar.set_show_text(True)
        content.pack_start(self.bar, False, False, 0)
        sw = Gtk.ScrolledWindow()
        sw.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        sw.set_size_request(-1, 360)
        self.log = Gtk.TextView()
        self.log.set_editable(False)
        self.log.set_cursor_visible(False)
        self.log.set_monospace(True)
        self.log.get_style_context().add_class('log-view')
        sw.add(self.log)
        content.pack_start(sw, True, True, 0)
        self.stack.add_named(page, 'progress')

    def _build_ready_page(self):
        Gtk = self.Gtk
        page, content = self._page('Ready to play',
                                   'The game is built. The first start prepares its shaders (about 15 minutes); '
                                   'later starts go straight to the game.')
        icon = Gtk.Image.new_from_icon_name('emblem-ok-symbolic', Gtk.IconSize.DIALOG)
        icon.set_halign(Gtk.Align.CENTER)
        content.pack_start(icon, False, False, 0)
        self.ready_status = Gtk.Label()
        self.ready_status.set_halign(Gtk.Align.CENTER)
        self.ready_status.set_line_wrap(True)
        content.pack_start(self.ready_status, False, False, 0)
        self.stack.add_named(page, 'ready')

    def _build_action_bar(self, outer):
        Gtk = self.Gtk
        self.action_bar = Gtk.ActionBar()
        self.action_bar.get_style_context().add_class('action-bar')
        self.about_btn = Gtk.Button(label='About')
        self.about_btn.connect('clicked', self._about)
        self.back_btn = Gtk.Button(label='Previous')
        self.back_btn.connect('clicked', self._previous)
        self.next_btn = Gtk.Button(label='Next')
        self.next_btn.get_style_context().add_class('suggested-action')
        self.next_btn.get_style_context().add_class('next')
        self.next_btn.connect('clicked', self._next)
        self.stop_btn = Gtk.Button(label='Stop')
        self.stop_btn.get_style_context().add_class('destructive-action')
        self.stop_btn.connect('clicked', self._stop)
        self.play_btn = Gtk.Button(label='Play')
        self.play_btn.get_style_context().add_class('suggested-action')
        self.play_btn.connect('clicked', self._play)
        self.open_log_btn = Gtk.Button(label='Open log')
        self.open_log_btn.connect('clicked', self._open_install_log)
        self.install_again_btn = Gtk.Button(label='Install again')
        self.install_again_btn.connect('clicked', self._install_again)
        center = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.step_label = Gtk.Label()
        self.step_label.get_style_context().add_class('step-label')
        self.help = Gtk.Label()
        self.help.set_line_wrap(True)
        self.help.set_max_width_chars(70)
        self.help.get_style_context().add_class('step-label')
        center.pack_start(self.step_label, False, False, 0)
        center.pack_start(self.help, False, False, 0)
        self.action_bar.set_center_widget(center)
        self.action_bar.pack_start(self.about_btn)
        self.action_bar.pack_start(self.back_btn)
        self.action_bar.pack_end(self.next_btn)
        self.action_bar.pack_end(self.stop_btn)
        self.action_bar.pack_end(self.play_btn)
        self.action_bar.pack_end(self.open_log_btn)
        self.action_bar.pack_end(self.install_again_btn)
        for b in (self.about_btn, self.back_btn, self.next_btn, self.stop_btn, self.play_btn,
                  self.open_log_btn, self.install_again_btn, self.step_label, self.help):
            b.set_no_show_all(True)
        outer.pack_end(self.action_bar, False, False, 0)

    # -------------------------------------------------------------------------------------------------- state
    def _load(self):
        c = self.cfg
        for key, entry in (('base', self.base), ('update', self.update), ('keys', self.keys),
                           ('firmware', self.fw), ('dump', self.dump)):
            if c.get(key):
                entry.set_text(c[key])
        if not self.keys.get_text():
            default = os.path.join(os.path.expanduser('~'), '.switch', 'prod.keys')
            if os.path.isfile(default):
                self.keys.set_text(default)
        self.storage_entry.set_text(self.storage)
        self.jobs.set_value(int(c.get('jobs', min(24, os.cpu_count() or 8))))
        for p, sub in ((self.base, self.base_sub), (self.update, self.update_sub), (self.keys, self.keys_sub),
                       (self.dump, self.dump_sub), (self.fw, self.fw_sub)):
            self._file_sub(p, sub)
        self._refresh_dlc_list()

    def _file_sub(self, entry, sub):
        p = entry.get_text().strip()
        if not p:
            sub.set_text('Not chosen')
            sub.set_tooltip_text(None)
            return
        name = os.path.basename(p.rstrip('/')) or p
        sub.set_text(name)
        sub.set_tooltip_text(p)

    def _set_row_visible(self, row, visible):
        """Show a row and its own children. The rows carry no_show_all so the window's initial show_all() does not
        reveal the wrong mode; gtk_widget_show_all() itself then skips a no_show_all widget, so clear the flag for
        this one call."""
        if visible:
            row.set_no_show_all(False)
            row.show_all()
            row.set_no_show_all(True)
        else:
            row.hide()

    def _sync(self):
        if not hasattr(self, 'base_row'):       # a radio button can toggle while the files page is still being built
            return
        packages = self.rb_packages.get_active()
        for row in (self.base_row, self.update_row, self.keys_row):
            self._set_row_visible(row, packages)
        self._set_row_visible(self.dump_row, not packages)
        self.files_help.set_text('' if packages else 'The folder must contain exefs/ and romfs/.')
        self._update_nav()
        self._refresh_review()

    def _on_mode(self, *_):
        self._sync()

    def _on_storage_text(self, *_):
        p = self.storage_entry.get_text().strip()
        if p:
            self.storage = os.path.abspath(p)
            self._refresh_storage()

    def _refresh_storage(self):
        self.storage_sub.set_text(self.storage)
        self.storage_sub.set_tooltip_text(self.storage)
        free = free_gb(self.storage)
        self.disk.set_text(('%.0f GB free in %s' % (free, self.storage)) if free is not None else self.storage)
        self._refresh_install()
        self._update_nav()

    def _refresh_review(self):
        if not hasattr(self, 'review'):
            return
        self.review['mode'].set_text('Package files' if self.rb_packages.get_active() else 'Existing dump folder')
        for key, entry in (('base', self.base), ('update', self.update), ('keys', self.keys), ('storage', self.storage_entry)):
            p = entry.get_text().strip()
            self.review[key].set_text(os.path.basename(p.rstrip('/')) if p else 'Not chosen')
            self.review[key].set_tooltip_text(p)
        if not self.keys.get_text().strip():
            default = os.path.join(os.path.expanduser('~'), '.switch', 'prod.keys')
            if os.path.isfile(default):
                self.review['keys'].set_text('Found in ~/.switch')
        self.review['dlc'].set_text('%d package(s)' % len(self.dlc) if self.dlc else 'None')
        self.review['jobs'].set_text(str(self.jobs.get_value_as_int()))

    def _refresh_install(self):
        self.info = read_json(os.path.join(self.storage, 'install.json'))
        exe = os.path.join(self.info.get('build', ''), 'work', 'ssbu_native') if self.info else ''
        ready = bool(self.info and os.path.isfile(exe))
        self.play_btn.set_sensitive(ready and self.proc is None)
        self.play_now_btn.set_visible(ready and self.proc is None)
        if ready:
            self.ready_status.set_text('Game version %s, built in %s.' % (
                self.info.get('game_version') or '13.0.5', os.path.dirname(self.info.get('build', ''))))
        else:
            self.ready_status.set_text('No finished build in this storage folder yet.')

    def _update_nav(self):
        Gtk = self.Gtk
        name = self.page_name
        setup = name in PAGES
        self.step_label.set_visible(setup)
        self.help.set_visible(setup)
        if setup:
            self.step = PAGES.index(name)
            self.step_label.set_text('Step %d of %d' % (self.step + 1, len(PAGES)))
            self.next_btn.set_label('Install' if name == 'review' else 'Next')
            self.next_btn.set_sensitive(self._validate())
        self.about_btn.set_visible(name == 'source')
        self.back_btn.set_visible((setup and self.step > 0) or (name == 'progress' and self.proc is None))
        self.back_btn.set_label('Back to review' if name == 'progress' else 'Previous')
        self.back_btn.set_sensitive(True)
        self.next_btn.set_visible(setup or (name == 'progress' and self.proc is None))
        if name == 'progress' and self.proc is None:
            self.next_btn.set_label('Retry')
            self.next_btn.set_sensitive(True)
        self.stop_btn.set_visible(name == 'progress' and self.proc is not None)
        self.stop_btn.set_sensitive(bool(self.proc))
        self.play_btn.set_visible(name == 'ready')
        self.play_btn.set_sensitive(self.play_btn.get_sensitive())
        self.open_log_btn.set_visible(name in ('progress', 'ready'))
        self.install_again_btn.set_visible(name == 'ready')
        self._set_help(self._help_text())

    def _help_text(self):
        if self.page_name in ('files', 'review'):
            if self.rb_packages.get_active():
                if not (self.base.get_text().strip() and self.update.get_text().strip()):
                    return 'Choose the base game and the update.'
                if not self.keys.get_text().strip() and not os.path.isfile(
                        os.path.join(os.path.expanduser('~'), '.switch', 'prod.keys')):
                    return 'Package files are encrypted: choose your prod.keys.'
            else:
                d = self.dump.get_text().strip()
                if not d or not os.path.isfile(os.path.join(d, 'exefs', 'main')):
                    return 'Choose a dump folder that contains exefs/ and romfs/.'
        if self.page_name in ('location', 'review') and not self.storage_entry.get_text().strip():
            return 'Choose a storage folder.'
        return ''

    def _validate(self):
        return not self._help_text()

    def _set_help(self, text):
        self.help.set_text(text)

    # -------------------------------------------------------------------------------------------------- file choosers
    def _chooser(self, title, action, multiple, filters, cb):
        Gtk = self.Gtk
        d = Gtk.FileChooserNative.new(title, self.win, action,
                                      'Open' if action != Gtk.FileChooserAction.SELECT_FOLDER else 'Choose', 'Cancel')
        d.set_select_multiple(multiple)
        for name, patterns in filters:
            f = Gtk.FileFilter()
            f.set_name(name)
            for p in patterns:
                f.add_pattern(p)
            d.add_filter(f)
        if action == Gtk.FileChooserAction.OPEN:
            f = Gtk.FileFilter()
            f.set_name('All files')
            f.add_pattern('*')
            d.add_filter(f)
        self._dialog = d
        d.connect('response', lambda dlg, resp: self._chooser_done(dlg, resp, multiple, cb))
        d.show()

    def _chooser_done(self, dlg, resp, multiple, cb):
        paths = None
        if resp == self.Gtk.ResponseType.ACCEPT:
            paths = list(dlg.get_filenames() or []) if multiple else ([dlg.get_filename()] if dlg.get_filename() else [])
        dlg.destroy()
        self._dialog = None
        if paths:
            cb(paths[0] if not multiple else paths)

    def _browse(self, _btn, entry, kind, tip):
        Gtk = self.Gtk
        if kind == 'dir':
            self._chooser(tip or 'Choose a folder', Gtk.FileChooserAction.SELECT_FOLDER, False, [],
                          lambda p: entry.set_text(p))
        elif kind == 'keys':
            self._chooser(tip or 'prod.keys', Gtk.FileChooserAction.OPEN, False,
                          [('Keys', ['*.keys'])], lambda p: entry.set_text(p))
        else:
            self._chooser(tip or 'Switch package', Gtk.FileChooserAction.OPEN, False,
                          [('Switch packages', ['*.nsz', '*.nsp', '*.xci', '*.NSZ', '*.NSP', '*.XCI'])],
                          lambda p: entry.set_text(p))

    def _add_dlc(self, *_):
        Gtk = self.Gtk
        self._chooser('DLC packages', Gtk.FileChooserAction.OPEN, True,
                      [('Switch packages', ['*.nsz', '*.nsp', '*.xci', '*.NSZ', '*.NSP', '*.XCI'])],
                      self._dlc_chosen)

    def _dlc_chosen(self, paths):
        for p in paths:
            if p not in self.dlc:
                self.dlc.append(p)
        self._refresh_dlc_list()
        self._refresh_review()

    def _clear_dlc(self, *_):
        self.dlc = []
        self._refresh_dlc_list()
        self._refresh_review()

    def _refresh_dlc_list(self):
        for child in self.dlc_list.get_children():
            self.dlc_list.remove(child)
        for p in self.dlc:
            row, _ = self._row(os.path.basename(p), p)
            self.dlc_list.add(row)
        self.dlc_list.show_all()

    # -------------------------------------------------------------------------------------------------- wizard
    def _show_step(self, name):
        self.page_name = name
        self.stack.set_visible_child_name(name)
        if name in PAGES:
            self.step = PAGES.index(name)
        if name == 'review':
            self._refresh_review()
        if name == 'location':
            self._refresh_storage()
        self._update_nav()

    def _next(self, *_):
        if self.page_name == 'progress':
            self._install()
            return
        if not self._validate():
            return
        if self.page_name == 'review':
            self._install()
            return
        i = PAGES.index(self.page_name)
        if i < len(PAGES) - 1:
            self._show_step(PAGES[i + 1])

    def _previous(self, *_):
        if self.page_name == 'progress':
            self._show_step('review')
            return
        if self.page_name in PAGES:
            i = PAGES.index(self.page_name)
            if i > 0:
                self._show_step(PAGES[i - 1])

    def _install_again(self, *_):
        self._show_step('source')

    # -------------------------------------------------------------------------------------------------- install
    def _install(self, *_):
        if self.proc:
            return
        if not self._validate():
            self._show_step('files' if self.page_name == 'review' else self.page_name)
            return
        packages = self.rb_packages.get_active()
        storage = self.storage_entry.get_text().strip()
        if not storage:
            self._show_step('location')
            self._set_help('Choose a storage folder.')
            return
        self.storage = os.path.abspath(storage)
        os.makedirs(self.storage, exist_ok=True)
        self.save_config()
        cmd = [sys.executable, os.path.join(REPO, 'scripts', 'linux', 'install_linux.py'),
               '--storage', self.storage, '--progress-json', '--jobs', str(self.jobs.get_value_as_int())]
        if packages:
            cmd += ['--base', self.base.get_text().strip(), '--update', self.update.get_text().strip()]
            keys = self.keys.get_text().strip()
            if keys:
                cmd += ['--keys', keys]
            fw = self.fw.get_text().strip()
            if fw:
                cmd += ['--firmware', fw]
        else:
            cmd += ['--dump', self.dump.get_text().strip()]
        for p in self.dlc:
            cmd += ['--dlc', p]
        self._clear_log()
        self._log('$ ' + ' '.join(cmd) + '\n')
        self.setup_state = {}
        self._mark(0, 'run')
        self._progress(0.0, 'Starting...')
        self.status.set_text('Starting the install...')
        self._show_step('progress')
        self.spinner.start()
        env = dict(os.environ, PYTHONUNBUFFERED='1')
        self.proc = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, encoding='utf-8', errors='replace', bufsize=1, start_new_session=True)
        self._update_nav()
        threading.Thread(target=self._reader, args=(self.proc,), daemon=True).start()

    def _reader(self, p):
        for line in p.stdout:
            self.q.put(('line', line.rstrip('\n')))
        self.q.put(('exit', p.wait()))

    def _pump(self):
        try:
            while True:
                kind, value = self.q.get_nowait()
                if kind == 'exit':
                    self._finish(value)
                elif kind == 'game':
                    self._game_done(value)
                else:
                    self._line(value)
        except queue.Empty:
            pass
        return True

    def _line(self, line):
        if line.startswith('##ssbu '):
            try:
                self._event(json.loads(line[7:]))
            except ValueError:
                pass
            return
        self._log(line + '\n')

    def _event(self, e):
        kind = e.get('event')
        if kind == 'step' and e.get('step') == 'dump':
            pct = float(e.get('pct') or 0) / 100.0
            self._progress(DUMP_BAR * pct, 'Unpacking the game files...')
        elif kind == 'step' and e.get('step') == 'setup':
            n = int(e.get('n') or 0)
            name = e.get('name') or ''
            before = sum(SETUP_W.get(s, 0) for s in SETUP_STEPS[:max(0, n - 1)])
            start = DUMP_BAR + (1.0 - DUMP_BAR) * before / SETUP_TOTAL
            end = DUMP_BAR + (1.0 - DUMP_BAR) * (before + SETUP_W.get(name, 0)) / SETUP_TOTAL
            self.setup_state = {'n': n, 'name': name, 'start': start, 'end': end}
            self._mark(0, 'ok')
            self._mark(1, 'run')
            if name == 'compile':
                self._progress(start, 'Compiling...')
                self.status.set_text('Compiling the game code. This can take 1-2 hours; the log below shows each '
                                     'file as it finishes, and a slow file can take minutes.')
            elif name == 'nro':
                self._progress(start, 'Compiling script modules...')
                self.status.set_text('Compiling the 98 script modules. The log below shows each module as it finishes.')
            elif name == 'lift':
                self._progress(start, 'Lifting the game code...')
                self.status.set_text('Lifting the game code (step %d of 11)...' % n)
            else:
                self._progress(start, 'Building: %s (step %d of 11)' % (name, n))
                self.status.set_text('Building: %s (step %d of 11)...' % (name, n))
        elif kind == 'compile':
            done, total = int(e.get('done') or 0), int(e.get('total') or 0)
            step = e.get('step') or self.setup_state.get('name')
            if step in ('compile', 'nro') and self.setup_state.get('name') == step:
                if total:
                    pct = self.setup_state['start'] + (self.setup_state['end'] - self.setup_state['start']) * done / total
                    text = 'Compiling %d/%d files' % (done, total)
                else:
                    pct = self.setup_state['start']
                    text = 'Compiling script modules'
                elapsed = int(e.get('elapsed') or 0)
                if elapsed:
                    text += ' — %dm%02ds elapsed' % (elapsed // 60, elapsed % 60)
                last = e.get('last')
                if last and e.get('last_seconds'):
                    text += ' (last slow file %s, %ss)' % (last, e.get('last_seconds'))
                self._progress(pct, text)
                self.status.set_text(text + ('.' if not text.endswith('.') else ''))
        elif kind == 'status' and e.get('text'):
            self.status.set_text(e['text'])
        elif kind == 'done':
            self._mark(0, 'ok')
            self._mark(1, 'ok')
            self._mark(2, 'ok')
            self._progress(1.0, 'Ready. Press Play.')
        elif kind == 'error' and e.get('text'):
            self.status.set_text(e['text'])

    def _finish(self, code):
        self.proc = None
        self.spinner.stop()
        if self.stopping:
            self.stopping = False
            self.status.set_text('Stopped. Press Retry or Back to review; the next Install continues.')
            self._show_step('progress')
        elif code == 0:
            self._mark(1, 'ok')
            self._mark(2, 'ok')
            self._progress(1.0, 'Ready. Press Play.')
            self._refresh_install()
            self._show_step('ready')
        else:
            self._mark(1, 'bad')
            self.status.set_text('Stopped (code %d). The log is in the storage folder; press Retry to continue.' % code)
            self._show_step('progress')
        self._update_nav()

    def _progress(self, fraction, text):
        self.bar.set_fraction(max(0.0, min(1.0, fraction)))
        self.bar.set_text(text)

    def _mark(self, i, state):
        sym = {'run': '▶ ', 'ok': '✔ ', 'bad': '✖ '}.get(state, '○ ')
        color = {'run': '#0a58ca', 'ok': '#198754', 'bad': '#dc3545'}.get(state, '#777777')
        names = ['Game files', 'Build', 'Ready']
        for k in range(i):
            if not self.progress_marks[k].get_text().startswith('✔'):
                self.progress_marks[k].set_markup('<span foreground="#198754">✔ %s</span>' % names[k])
        self.progress_marks[i].set_markup('<span foreground="%s">%s %s</span>' % (color, sym, names[i]))

    def _stop(self, *_):
        if not self.proc:
            return
        self.stopping = True
        self.stop_btn.set_sensitive(False)
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
        except OSError:
            pass
        self.status.set_text('Stopping...')

    # -------------------------------------------------------------------------------------------------- play
    def _play(self, *_):
        if self.game_proc or self.proc:
            return
        info = read_json(os.path.join(self.storage, 'install.json'))
        if not info:
            return self._warn('There is no finished build in %s: press Install first.' % self.storage)
        exe = os.path.join(info.get('build', ''), 'work', 'ssbu_native')
        if not os.path.isfile(exe):
            return self._warn('The game program is missing: press Install again (it continues where it stopped).')
        log = os.path.join(self.storage, 'last_run.log')
        cmd = [sys.executable, os.path.join(REPO, 'scripts', 'run.py'), '--linux', '--build', info['build'],
               '--play', '--userdata', info.get('userdata') or os.path.join(self.storage, 'userdata'), '--log', log]
        self._log('$ ' + ' '.join(cmd) + '\n')
        env = dict(os.environ)
        env['SSBU_PACKAGED'] = '1'
        try:
            self.game_proc = subprocess.Popen(cmd, cwd=REPO, env=env, start_new_session=True)
        except OSError as e:
            return self._warn('The game could not start: %s' % e)
        self.win.hide()
        threading.Thread(target=lambda: self.q.put(('game', self.game_proc.wait())), daemon=True).start()

    def _game_done(self, code):
        self.game_proc = None
        self.win.show_all()
        self._show_step('ready')
        self.play_btn.set_sensitive(True)
        self.status.set_text('Game closed%s. Press Play to start it again.' % ('' if code == 0 else ' (code %d)' % code))
        if code != 0:
            self._log('game exited with code %d; the game log is %s\n' % (code, os.path.join(self.storage, 'last_run.log')))

    # -------------------------------------------------------------------------------------------------- misc
    def _clear_log(self):
        self.log.get_buffer().set_text('')

    def _log(self, text):
        buf = self.log.get_buffer()
        buf.insert(buf.get_end_iter(), text)
        mark = buf.create_mark(None, buf.get_end_iter(), False)
        self.log.scroll_mark_onscreen(mark)
        buf.delete_mark(mark)

    def _open_storage(self):
        self._open_path(self.storage)

    def _open_log(self):
        self._open_path(os.path.join(self.storage, 'install.log'))

    def _open_install_log(self):
        self._open_log()

    def _open_path(self, path):
        try:
            uri = self.Gio.File.new_for_path(os.path.abspath(path)).get_uri()
            self.Gtk.show_uri_on_window(self.win, uri, self.Gtk.get_current_event_time())
        except Exception as e:
            self._warn('Could not open %s: %s' % (path, e))

    def _about(self, *_):
        d = self.Gtk.AboutDialog(parent=self.win)
        d.set_program_name('OpenSmash Ultimate')
        d.set_version('0.1.0')
        d.set_comments('Build and play Super Smash Bros. Ultimate from your own dump.')
        d.set_license_type(self.Gtk.License.GPL_3_0)
        d.set_website('https://denkishi.github.io/opensmash-site/')
        d.set_logo_icon_name('io.github.Rsoto95.OpenSmashUltimate')
        d.run()
        d.destroy()

    def _warn(self, text):
        d = self.Gtk.MessageDialog(parent=self.win, flags=0, message_type=self.Gtk.MessageType.WARNING,
                                   buttons=self.Gtk.ButtonsType.OK, text=text)
        d.run()
        d.destroy()

    def save_config(self):
        c = {'mode': 'packages' if self.rb_packages.get_active() else 'dump',
             'base': self.base.get_text(), 'update': self.update.get_text(), 'keys': self.keys.get_text(),
             'firmware': self.fw.get_text(), 'dump': self.dump.get_text(), 'dlc': self.dlc,
             'storage': self.storage, 'jobs': self.jobs.get_value_as_int()}
        try:
            os.makedirs(os.path.dirname(config_path()), exist_ok=True)
            with open(config_path(), 'w', encoding='utf-8') as f:
                json.dump(c, f, indent=1)
        except OSError:
            pass

    def _on_close(self, *_):
        running = self.proc or self.game_proc
        if running:
            d = self.Gtk.MessageDialog(parent=self.win, flags=0, message_type=self.Gtk.MessageType.QUESTION,
                                       buttons=self.Gtk.ButtonsType.YES_NO,
                                       text='Stop the running game/build and close?')
            resp = d.run()
            d.destroy()
            if resp != self.Gtk.ResponseType.YES:
                return True
            for p in (self.proc, self.game_proc):
                if p:
                    try:
                        os.killpg(os.getpgid(p.pid), signal.SIGTERM)
                    except OSError:
                        pass
        self.save_config()
        self.Gtk.main_quit()
        return False


if __name__ == '__main__':
    sys.exit(main())
