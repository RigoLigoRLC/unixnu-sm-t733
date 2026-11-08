import AppKit

// MenuApp displays the local helper's state and sends commands through mscctl.
// Terminal owns privileged startup; this application runs as the desktop user.
final class MenuApp: NSObject, NSApplicationDelegate {
    private var item: NSStatusItem!
    private var statusLine: NSMenuItem!
    private var timer: Timer?
    private var polling = false

    private var resources: URL {
        return Bundle.main.resourceURL!
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.title = "T733 MSC"

        let menu = NSMenu()
        statusLine = NSMenuItem(title: "Connecting...", action: nil, keyEquivalent: "")
        menu.addItem(statusLine)
        menu.addItem(NSMenuItem.separator())
        add(menu, "Start service in Terminal", #selector(startService))
        add(menu, "List tablet", #selector(listDevice))
        add(menu, "Write FAT image...", #selector(writeFat))
        add(menu, "Read FAT image...", #selector(readFat))
        add(menu, "Show task log", #selector(showLog))
        add(menu, "Eject tablet", #selector(eject))
        add(menu, "Stop service", #selector(stopService))
        menu.addItem(NSMenuItem.separator())
        add(menu, "Quit menu app", #selector(quit))
        item.menu = menu

        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.refresh()
        }
        refresh()
    }

    private func add(_ menu: NSMenu, _ title: String, _ selector: Selector) {
        let entry = NSMenuItem(title: title, action: selector, keyEquivalent: "")
        entry.target = self
        menu.addItem(entry)
    }

    private func run(_ arguments: [String], completion: @escaping (String, Bool) -> Void) {
        let executable = resources.appendingPathComponent("mscctl")
        DispatchQueue.global(qos: .utility).async {
            let process = Process()
            let pipe = Pipe()
            process.executableURL = executable
            process.arguments = arguments
            process.standardOutput = pipe
            process.standardError = pipe

            do {
                try process.run()
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                process.waitUntilExit()
                let output = String(data: data, encoding: .utf8) ?? "No output"
                let success = process.terminationStatus == 0
                DispatchQueue.main.async {
                    completion(output, success)
                }
            } catch {
                let message = error.localizedDescription
                DispatchQueue.main.async {
                    completion(message, false)
                }
            }
        }
    }

    private func refresh() {
        guard !polling else {
            return
        }
        polling = true
        run(["status"]) { [weak self] text, success in
            guard let self = self else {
                return
            }
            self.polling = false

            guard success,
                  let data = text.data(using: .utf8),
                  let state = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                self.item.button?.title = "T733 MSC: Off"
                self.statusLine.title = "Service stopped"
                return
            }

            let operation = state["operation"] as? String ?? "idle"
            let result = state["result"] as? String ?? "ready"
            let bytes = state["bytes"] as? Int ?? 0
            let total = state["total"] as? Int ?? 0
            if result == "running" && total > 0 {
                self.item.button?.title = "T733 MSC: \(bytes * 100 / total)%"
            } else {
                self.item.button?.title = "T733 MSC: On"
            }
            self.statusLine.title = "\(operation): \(result)"
        }
    }

    private func command(_ arguments: [String]) {
        run(arguments) { [weak self] output, success in
            self?.refresh()
            let alert = NSAlert()
            alert.messageText = success ? "T733 MSC completed" : "T733 MSC failed"
            alert.informativeText = String(output.suffix(6000))
            alert.runModal()
        }
    }

    @objc private func startService() {
        NSWorkspace.shared.open(resources.appendingPathComponent("Start Service.command"))
    }

    @objc private func listDevice() {
        command(["list"])
    }

    @objc private func writeFat() {
        let panel = NSOpenPanel()
        panel.message = "Choose the completed 512 MiB T733ESP image"
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else {
            return
        }
        command(["write-fat", url.path])
    }

    @objc private func readFat() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "t733-fat.img"
        panel.message = "Choose a new file for the 512 MiB FAT image"
        guard panel.runModal() == .OK, let url = panel.url else {
            return
        }
        command(["read-fat", url.path])
    }

    @objc private func showLog() {
        command(["logs"])
    }

    @objc private func eject() {
        command(["eject"])
    }

    @objc private func stopService() {
        command(["shutdown"])
    }

    @objc private func quit() {
        NSApplication.shared.terminate(nil)
    }
}

let application = NSApplication.shared
let delegate = MenuApp()
application.delegate = delegate
application.setActivationPolicy(.accessory)
application.run()
