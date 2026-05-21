cask "cdripper" do
  version "1.7.8"
  sha256 "bccfdaae759dae81d11f0eb0f3dcb9207fb198e7d784b2b75857a75fec3332be"

  url "https://flatpak.x2-pandora.de/cdripper/cdripper-#{version}-macos-x86_64.dmg"
  name "CD Ripper"
  desc "Audio CD ripper with MusicBrainz tagging and AccurateRip verification"
  homepage "https://github.com/Gravey158/cdripper"

  # Sparkle-AppCast als Quelle für brew livecheck — bei `brew outdated --cask`
  # sieht brew die neueste Version direkt aus unserem Update-Feed.
  livecheck do
    url "https://flatpak.x2-pandora.de/cdripper/appcast.xml"
    strategy :sparkle
  end

  auto_updates true
  depends_on macos: ">= :sonoma"
  depends_on formula: [
    "libcdio",
    "libcdio-paranoia",
    "libdiscid",
    "flac",
    "opus-tools",
    "lame",
    "rsgain",
    "chromaprint",
    "ffmpeg",
  ]

  app "cdripper.app"

  # Adhoc-signed (kein Apple Developer ID) → macOS Gatekeeper zeigt sonst
  # „cdripper ist beschädigt" beim Doppelklick. Postflight cleart das
  # quarantine xattr via sudo (Cask prompted dann einmalig nach Passwort).
  # `must_succeed: false`: wenn der User das Passwort verweigert, schlägt
  # der Install nicht fehl — der Hinweis aus den `caveats` greift dann.
  postflight do
    system_command "/usr/bin/xattr",
                   args:         ["-cr", "#{appdir}/cdripper.app"],
                   sudo:         true,
                   must_succeed: false
  end

  caveats <<~EOS
    cdripper ist ad-hoc-signiert (kein Apple Developer Certificate).

    Falls macOS „cdripper.app ist beschädigt" anzeigt:
      sudo xattr -cr /Applications/cdripper.app

    Alternative: im Finder Rechtsklick auf cdripper.app → Öffnen → im Dialog
    „Öffnen" bestätigen. Macht einen Gatekeeper-Override für diese App.
  EOS

  zap trash: [
    "~/.config/cdripper",
    "~/.local/share/cdripper",
    "~/Library/Preferences/io.github.gravey158.cdripper.plist",
    "~/Library/Caches/io.github.gravey158.cdripper",
    "~/Library/Application Support/io.github.gravey158.cdripper",
  ]
end
