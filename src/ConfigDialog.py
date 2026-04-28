#!/usr/bin/env python3
import sys
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk

class ConfigDialog(Gtk.Window):
    def __init__(self, title, fields, current_values):
        super().__init__(title=title)
        self.set_border_width(10)
        self.set_default_size(450, -1)
        self.set_resizable(False)
        self.result = None

        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        self.add(vbox)

        grid = Gtk.Grid(column_spacing=10, row_spacing=10)
        vbox.pack_start(grid, True, True, 0)

        self.widgets = {}
        for i, (key, label, type) in enumerate(fields):
            lbl = Gtk.Label(label=label, xalign=0)
            grid.attach(lbl, 0, i, 1, 1)

            value = current_values.get(key, "")

            if type == "entry":
                widget = Gtk.Entry()
                widget.set_text(str(value))
                grid.attach(widget, 1, i, 1, 1)
                self.widgets[key] = (widget, "entry")
            elif type == "switch":
                widget = Gtk.Switch()
                widget.set_active(str(value).lower() == "true")
                hbox = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
                hbox.pack_start(widget, False, False, 0)
                grid.attach(hbox, 1, i, 1, 1)
                self.widgets[key] = (widget, "switch")
            elif type == "combo":
                options = label.split("|") # Hack to pass options
                lbl.set_text(options[0])
                widget = Gtk.ComboBoxText()
                for opt in options[1:]:
                    widget.append_text(opt)

                # Find current index
                active_idx = 0
                for idx, opt in enumerate(options[1:]):
                    if opt == value:
                        active_idx = idx
                        break
                widget.set_active(active_idx)
                grid.attach(widget, 1, i, 1, 1)
                self.widgets[key] = (widget, "combo")

        hbuttonbox = Gtk.ButtonBox(spacing=10, layout_style=Gtk.ButtonBoxStyle.END)
        vbox.pack_start(hbuttonbox, False, False, 0)

        btn_cancel = Gtk.Button(label="Cancel")
        btn_cancel.connect("clicked", self.on_cancel)
        hbuttonbox.add(btn_cancel)

        btn_save = Gtk.Button(label="Save")
        btn_save.connect("clicked", self.on_save)
        btn_save.get_style_context().add_class("suggested-action")
        hbuttonbox.add(btn_save)

        self.connect("destroy", Gtk.main_quit)

    def on_save(self, btn):
        res = []
        for key, (widget, type) in self.widgets.items():
            if type == "entry":
                res.append(widget.get_text())
            elif type == "switch":
                res.append("true" if widget.get_active() else "false")
            elif type == "combo":
                res.append(widget.get_active_text())
        self.result = "|".join(res)
        self.destroy()

    def on_cancel(self, btn):
        self.destroy()

def main():
    if len(sys.argv) < 4:
        print("Usage: settings_gui.py TITLE FIELDS VALUES")
        sys.exit(1)

    title = sys.argv[1]
    raw_fields = sys.argv[2].split("||")
    raw_values = sys.argv[3].split("|")

    fields = []
    current_values = {}
    for i, field in enumerate(raw_fields):
        name, label, type = field.split(":")
        fields.append((name, label, type))
        if i < len(raw_values):
            current_values[name] = raw_values[i]

    win = ConfigDialog(title, fields, current_values)
    win.show_all()
    Gtk.main()

    if win.result:
        print(win.result)
        sys.exit(0)
    else:
        sys.exit(1)

if __name__ == "__main__":
    main()
