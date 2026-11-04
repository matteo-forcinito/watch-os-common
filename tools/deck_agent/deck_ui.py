"""Watch Deck window: status, quick launch and own shortcuts (tkinter).

Runs as its own process ("WatchDeck.exe --ui", or "python deck_ui.py"): it only
edits the settings file (deck_config.py) and reads the agent's status file,
so it never touches the Bluetooth link. The agent picks up a save within two
seconds and updates the watches.
"""

from __future__ import annotations

import json
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import deck_config

TITLE = "Watch Deck"
PAD = 10


class Window:
    def __init__(self) -> None:
        self.config = deck_config.load()
        self.apps: list[dict] = []  # installed apps, filled in the background (PowerShell takes a moment)
        self.root = tk.Tk()
        self.root.title(TITLE)
        self.root.geometry("760x520")
        self.root.minsize(640, 440)
        style = ttk.Style(self.root)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Title.TLabel", font=("Segoe UI", 16, "bold"))
        style.configure("Dim.TLabel", foreground="#666")

        tabs = ttk.Notebook(self.root)
        tabs.pack(fill="both", expand=True, padx=PAD, pady=PAD)
        tabs.add(self.status_tab(tabs), text="Stato")
        tabs.add(self.launch_tab(tabs), text="Avvio rapido")
        tabs.add(self.keys_tab(tabs), text="Scorciatoie")
        tabs.add(self.dev_tab(tabs), text="Git e CI")
        threading.Thread(target=self.find_apps, daemon=True).start()
        self.refresh_status()

    # -------------------------------------------------------------- status

    def status_tab(self, parent) -> ttk.Frame:
        frame = ttk.Frame(parent, padding=PAD * 2)
        ttk.Label(frame, text=TITLE, style="Title.TLabel").pack(anchor="w")
        self.state = ttk.Label(frame, text="", font=("Segoe UI", 11))
        self.state.pack(anchor="w", pady=(PAD, 0))
        self.app_line = ttk.Label(frame, text="", style="Dim.TLabel")
        self.app_line.pack(anchor="w", pady=(4, 0))
        ttk.Label(frame, style="Dim.TLabel", justify="left", wraplength=640, text=(
            "Sul watch, nella pagina PC (Deck), tocca il titolo per le sezioni:\n"
            "  Le mie - le tue scorciatoie per l'app in primo piano (scheda Scorciatoie)\n"
            "  App - i tasti già pronti per l'app in primo piano\n"
            "  Git - con VS Code davanti: pull, push, commit, branch del repository aperto\n"
            "  CI - stato delle GitHub Actions dei repository scelti (scheda Git e CI)\n"
            "  Avvio - app e siti da aprire con un tocco (scheda Avvio rapido)\n"
            "  Sistema - cambia app, finestre aperte, desktop, blocco, sospensione\n\n"
            "Le modifiche arrivano al watch da sole in un paio di secondi.")).pack(anchor="w", pady=(PAD * 2, 0))
        return frame

    def refresh_status(self) -> None:
        try:
            with open(deck_config.STATUS_PATH, encoding="utf-8") as f:
                status = json.load(f)
        except (OSError, ValueError):
            status = {}
        if status.get("connected"):
            self.state.config(text=f"●  Collegato a {status.get('device', '?')} via {status.get('via', '?')}",
                              foreground="#1a7f37")
            self.app_line.config(text=f"Touchbar: {status.get('app') or '-'}")
        else:
            self.state.config(text="○  In attesa del watch (Remote aperta, USB o Bluetooth)", foreground="#666")
            self.app_line.config(text="")
        recent = status.get("recent") or []
        if recent and hasattr(self, "app_box"):
            known = sorted(set(recent) | set(self.config["keys"]))
            if list(self.app_box["values"]) != known:
                self.app_box["values"] = known
        self.root.after(1500, self.refresh_status)

    # -------------------------------------------------------------- quick launch

    def launch_tab(self, parent) -> ttk.Frame:
        frame = ttk.Frame(parent, padding=PAD)
        frame.columnconfigure(0, weight=1)
        frame.columnconfigure(2, weight=1)
        frame.rowconfigure(1, weight=1)

        ttk.Label(frame, text="Sul watch (max 16, in quest'ordine)").grid(row=0, column=0, sticky="w")
        self.chosen = tk.Listbox(frame, activestyle="none", font=("Segoe UI", 10))
        self.chosen.grid(row=1, column=0, sticky="nsew")
        self.chosen.bind("<Double-Button-1>", lambda _: self.remove_launch())

        buttons = ttk.Frame(frame, padding=(PAD, 0))
        buttons.grid(row=1, column=1, sticky="ns")
        ttk.Button(buttons, text="← Aggiungi", command=self.add_launch).pack(fill="x", pady=(40, 4))
        ttk.Button(buttons, text="Rimuovi", command=self.remove_launch).pack(fill="x", pady=4)
        ttk.Button(buttons, text="↑ Su", command=lambda: self.move_launch(-1)).pack(fill="x", pady=(20, 4))
        ttk.Button(buttons, text="↓ Giù", command=lambda: self.move_launch(1)).pack(fill="x", pady=4)
        ttk.Button(buttons, text="Prova", command=self.try_launch).pack(fill="x", pady=(20, 4))

        top = ttk.Frame(frame)
        top.grid(row=0, column=2, sticky="ew")
        ttk.Label(top, text="Disponibili").pack(side="left")
        self.search = tk.StringVar()
        self.search.trace_add("write", lambda *_: self.fill_available())
        ttk.Entry(top, textvariable=self.search).pack(side="right", fill="x", expand=True, padx=(PAD, 0))
        self.available = tk.Listbox(frame, activestyle="none", font=("Segoe UI", 10))
        self.available.grid(row=1, column=2, sticky="nsew")
        self.available.bind("<Double-Button-1>", lambda _: self.add_launch())

        site = ttk.Frame(frame, padding=(0, PAD, 0, 0))
        site.grid(row=2, column=0, columnspan=3, sticky="ew")
        ttk.Label(site, text="Sito:").pack(side="left")
        self.site_name = ttk.Entry(site, width=18)
        self.site_name.pack(side="left", padx=4)
        self.site_url = ttk.Entry(site)
        self.site_url.insert(0, "https://")
        self.site_url.pack(side="left", fill="x", expand=True, padx=4)
        ttk.Button(site, text="Aggiungi sito", command=self.add_site).pack(side="left")
        self.fill_chosen()
        self.fill_available()
        return frame

    def find_apps(self) -> None:
        apps = deck_config.installed_apps()
        sites = [{"name": n, "target": u} for n, u in deck_config.SITES]
        self.root.after(0, lambda: (setattr(self, "apps", apps + sites), self.fill_available()))

    def fill_chosen(self) -> None:
        self.chosen.delete(0, "end")
        for item in self.config["launch"]:
            web = item["target"].startswith("http")
            self.chosen.insert("end", ("\U0001F310  " if web else "▶  ") + item["name"])

    def fill_available(self) -> None:
        text = self.search.get().lower()
        self.shown = [a for a in self.apps if text in a["name"].lower()] if self.apps else []
        self.available.delete(0, "end")
        if not self.apps:
            self.available.insert("end", "Cerco le app installate...")
        for app in self.shown:
            web = app["target"].startswith("http")
            self.available.insert("end", ("\U0001F310  " if web else "▶  ") + app["name"])

    def save(self) -> None:
        deck_config.save(self.config)

    def add_launch(self) -> None:
        picked = self.available.curselection()
        if not picked or not self.apps:
            return
        if len(self.config["launch"]) >= deck_config.LAUNCH_MAX:
            messagebox.showinfo(TITLE, "Il watch mostra al massimo 16 elementi: rimuovine uno prima.")
            return
        app = self.shown[picked[0]]
        if any(item["target"] == app["target"] for item in self.config["launch"]):
            return
        self.config["launch"].append(dict(app))
        self.save()
        self.fill_chosen()

    def add_site(self) -> None:
        name, url = self.site_name.get().strip(), self.site_url.get().strip()
        if not name or not url.startswith(("http://", "https://")) or len(url) < 12:
            messagebox.showinfo(TITLE, "Scrivi un nome e un indirizzo che inizia con https://")
            return
        if len(self.config["launch"]) >= deck_config.LAUNCH_MAX:
            messagebox.showinfo(TITLE, "Il watch mostra al massimo 16 elementi: rimuovine uno prima.")
            return
        self.config["launch"].append({"name": name, "target": url})
        self.save()
        self.fill_chosen()
        self.site_name.delete(0, "end")

    def remove_launch(self) -> None:
        picked = self.chosen.curselection()
        if picked:
            del self.config["launch"][picked[0]]
            self.save()
            self.fill_chosen()

    def move_launch(self, delta: int) -> None:
        picked = self.chosen.curselection()
        if not picked:
            return
        i, j = picked[0], picked[0] + delta
        items = self.config["launch"]
        if 0 <= j < len(items):
            items[i], items[j] = items[j], items[i]
            self.save()
            self.fill_chosen()
            self.chosen.selection_set(j)

    def try_launch(self) -> None:
        picked = self.chosen.curselection()
        if picked:
            deck_config.launch(self.config["launch"][picked[0]]["target"])

    # -------------------------------------------------------------- own shortcuts

    def keys_tab(self, parent) -> ttk.Frame:
        frame = ttk.Frame(parent, padding=PAD)
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(2, weight=1)

        top = ttk.Frame(frame)
        top.grid(row=0, column=0, sticky="ew")
        ttk.Label(top, text="App (nome del processo):").pack(side="left")
        self.app_box = ttk.Combobox(top, values=sorted(self.config["keys"]), width=28)
        self.app_box.pack(side="left", padx=PAD)
        self.app_box.bind("<<ComboboxSelected>>", lambda _: self.fill_keys())
        self.app_box.bind("<Return>", lambda _: self.fill_keys())
        ttk.Label(frame, style="Dim.TLabel", wraplength=700, text=(
            "Le app usate di recente compaiono nell'elenco. Le scorciatoie appaiono sul watch nella sezione "
            "«Le mie» quando quell'app è in primo piano; il PC preme i tasti. Esempi: ctrl+shift+p, win+e, "
            "alt+f4, ctrl+alt+t, f5.")).grid(row=1, column=0, sticky="w", pady=(4, PAD))

        self.keys = ttk.Treeview(frame, columns=("label", "keys"), show="headings", height=8)
        self.keys.heading("label", text="Nome sul watch")
        self.keys.heading("keys", text="Tasti")
        self.keys.column("label", width=260)
        self.keys.grid(row=2, column=0, sticky="nsew")

        add = ttk.Frame(frame, padding=(0, PAD, 0, 0))
        add.grid(row=3, column=0, sticky="ew")
        ttk.Label(add, text="Nome:").pack(side="left")
        self.key_label = ttk.Entry(add, width=22)
        self.key_label.pack(side="left", padx=4)
        ttk.Label(add, text="Tasti:").pack(side="left")
        self.key_combo = ttk.Entry(add, width=22)
        self.key_combo.pack(side="left", padx=4)
        ttk.Button(add, text="Aggiungi", command=self.add_key).pack(side="left", padx=4)
        ttk.Button(add, text="Rimuovi", command=self.remove_key).pack(side="left", padx=4)
        ttk.Button(add, text="Prova", command=self.try_key).pack(side="left", padx=4)
        return frame

    def current_app(self) -> str:
        return self.app_box.get().strip().lower()

    def fill_keys(self) -> None:
        self.keys.delete(*self.keys.get_children())
        for item in self.config["keys"].get(self.current_app(), []):
            self.keys.insert("", "end", values=(item["label"], item["keys"]))

    def add_key(self) -> None:
        app, label, keys = self.current_app(), self.key_label.get().strip(), self.key_combo.get().strip().lower()
        if not app:
            messagebox.showinfo(TITLE, "Scegli prima l'app (es. code, chrome, excel).")
            return
        if not label or not deck_config.valid_keys(keys):
            messagebox.showinfo(TITLE, "Scrivi un nome e dei tasti validi, es. ctrl+shift+p")
            return
        items = self.config["keys"].setdefault(app, [])
        if len(items) >= deck_config.KEYS_MAX:
            messagebox.showinfo(TITLE, "Al massimo 16 scorciatoie per app.")
            return
        items.append({"label": label[:30], "keys": keys})
        self.save()
        self.fill_keys()
        self.key_label.delete(0, "end")
        self.key_combo.delete(0, "end")
        self.app_box["values"] = sorted(set(self.app_box["values"]) | {app})

    def remove_key(self) -> None:
        picked = self.keys.selection()
        app = self.current_app()
        if picked and app in self.config["keys"]:
            del self.config["keys"][app][self.keys.index(picked[0])]
            if not self.config["keys"][app]:
                del self.config["keys"][app]
            self.save()
            self.fill_keys()

    def try_key(self) -> None:
        keys = self.key_combo.get().strip()
        if deck_config.valid_keys(keys):
            messagebox.showinfo(TITLE, "Dopo OK clicca la finestra dell'app: premo i tasti tra 2 secondi.")
            self.root.after(2000, lambda: deck_config.press(keys))  # time to click the target window

    # -------------------------------------------------------------- git and CI

    def dev_tab(self, parent) -> ttk.Frame:
        frame = ttk.Frame(parent, padding=PAD)
        frame.columnconfigure(0, weight=1)
        ttk.Label(frame, text="Repository GitHub da seguire (proprietario/nome)").grid(row=0, column=0, sticky="w")
        self.ci_list = tk.Listbox(frame, height=6, activestyle="none")
        self.ci_list.grid(row=1, column=0, sticky="nsew")
        row = ttk.Frame(frame, padding=(0, 4, 0, 0))
        row.grid(row=2, column=0, sticky="ew")
        self.ci_entry = ttk.Entry(row)
        self.ci_entry.pack(side="left", fill="x", expand=True)
        ttk.Button(row, text="Aggiungi", command=self.add_ci).pack(side="left", padx=4)
        ttk.Button(row, text="Rimuovi", command=self.remove_ci).pack(side="left")
        ttk.Label(frame, style="Dim.TLabel", wraplength=700, justify="left", text=(
            "Il repository aperto in VS Code si segue da solo se ha un remote GitHub. Il token è quello "
            "che usa git (git credential): nessuna password da scrivere qui. Controllo ogni 60 s, ogni "
            "15 s mentre una run è in corso; il watch suona quando una run finisce.")).grid(
            row=3, column=0, sticky="w", pady=(4, PAD * 2))
        ttk.Label(frame, text="Cartelle di VS Code (se il watch non trova il repository)").grid(row=4, column=0, sticky="w")
        self.path_list = tk.Listbox(frame, height=5, activestyle="none")
        self.path_list.grid(row=5, column=0, sticky="nsew")
        buttons = ttk.Frame(frame, padding=(0, 4, 0, 0))
        buttons.grid(row=6, column=0, sticky="ew")
        ttk.Button(buttons, text="Aggiungi cartella...", command=self.add_path).pack(side="left")
        ttk.Button(buttons, text="Rimuovi", command=self.remove_path).pack(side="left", padx=4)
        frame.rowconfigure(1, weight=1)
        frame.rowconfigure(5, weight=1)
        self.fill_dev()
        return frame

    def fill_dev(self) -> None:
        self.ci_list.delete(0, "end")
        for slug in self.config.get("ci_repos", []):
            self.ci_list.insert("end", slug)
        self.path_list.delete(0, "end")
        for folder, path in sorted((self.config.get("git_paths") or {}).items()):
            self.path_list.insert("end", f"{folder}  →  {path}")

    def add_ci(self) -> None:
        slug = self.ci_entry.get().strip().removeprefix("https://github.com/").strip("/")
        if slug.endswith(".git"):
            slug = slug[:-4]
        if slug.count("/") != 1 or " " in slug:
            messagebox.showinfo(TITLE, "Scrivi proprietario/nome, per esempio matteo-forcinito/watch-os-common")
            return
        repos = self.config.setdefault("ci_repos", [])
        if slug not in repos:
            if len(repos) >= 8:
                messagebox.showinfo(TITLE, "Il watch segue al massimo 8 repository.")
                return
            repos.append(slug)
            self.save()
        self.ci_entry.delete(0, "end")
        self.fill_dev()

    def remove_ci(self) -> None:
        picked = self.ci_list.curselection()
        if picked:
            del self.config["ci_repos"][picked[0]]
            self.save()
            self.fill_dev()

    def add_path(self) -> None:
        path = filedialog.askdirectory(title="Cartella aperta in VS Code")
        if path:
            self.config.setdefault("git_paths", {})[path.replace("\\", "/").rstrip("/").split("/")[-1]] = path
            self.save()
            self.fill_dev()

    def remove_path(self) -> None:
        picked = self.path_list.curselection()
        if picked:
            folder = sorted(self.config.get("git_paths") or {})[picked[0]]
            del self.config["git_paths"][folder]
            self.save()
            self.fill_dev()

    def run(self) -> None:
        self.root.mainloop()


def main() -> None:
    Window().run()


if __name__ == "__main__":
    main()
