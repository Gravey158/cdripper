# cdripper Homebrew Cask

Master-Quelle der Cask-Formel. Spiegelt sich in `github.com/Gravey158/homebrew-cdripper`,
das offizielle Tap-Repo für `brew tap gravey158/cdripper`.

## User-Installation (nach Tap-Setup)

```sh
brew tap gravey158/cdripper
brew install --cask cdripper
```

Brew zieht `cdripper-VERSION-macos-x86_64.dmg` von `flatpak.x2-pandora.de`,
mountet, kopiert `cdripper.app` nach `/Applications`, und installiert die
benötigten Homebrew-Formula-Deps (libcdio, libcdio-paranoia, flac, opus-tools,
lame, rsgain, chromaprint, ffmpeg).

Updates kommen über `brew upgrade --cask cdripper` ODER über das integrierte
Sparkle-Update aus der App selbst (24h-Check).

## Pflege bei jedem Release

`scripts/publish-mac-release.sh <version>` automatisiert:

1. Mac-Build → `.dmg`
2. `sign_update` → EdDSA-Signatur
3. AppCast-Eintrag ergänzen + DMG hochladen auf flatpak.x2-pandora.de
4. `homebrew/Casks/cdripper.rb` mit neuer Version + SHA256 patchen
5. In `github.com/Gravey158/homebrew-cdripper` syncen + pushen

## Erst-Setup des Tap-Repos (einmalig)

Da auf diesem Mac kein `gh`-CLI installiert ist:

1. Auf github.com manuell ein **leeres, privates oder öffentliches**
   Repo `Gravey158/homebrew-cdripper` anlegen.
2. Lokal initialisieren + ersten Cask pushen:

```sh
mkdir -p ~/projects/homebrew-cdripper/Casks
cp homebrew/Casks/cdripper.rb ~/projects/homebrew-cdripper/Casks/
cd ~/projects/homebrew-cdripper
git init
git add Casks/cdripper.rb
git commit -m "1.7.4 — first Mac cask"
git branch -M main
git remote add origin https://github.com/Gravey158/homebrew-cdripper.git
git push -u origin main
```

3. Danach `brew tap gravey158/cdripper` funktioniert weltweit.
