cask "cdripper" do
  version "1.7.7"
  sha256 "152b8a0860c65eccb32234ebfd365f9a6044903d75eb92bf72686126573cec42"

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

  zap trash: [
    "~/.config/cdripper",
    "~/.local/share/cdripper",
    "~/Library/Preferences/io.github.gravey158.cdripper.plist",
    "~/Library/Caches/io.github.gravey158.cdripper",
    "~/Library/Application Support/io.github.gravey158.cdripper",
  ]
end
