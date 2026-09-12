#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify every committed and generated Phipia desktop asset."""

from hashlib import sha256
from pathlib import Path


PINNED = {
    "assets/phipia/logo.png": "6a07abe324c2d80aa0f1dd3a318c103c3b6a81fef1f72d5f4808d589626b1e88",
    "assets/phipia/wallpaper.png": "ce1df11fa3a5575b55b47a2fba7216216b21ce09c53824d85610515848184981",
    "build/logo.srl": "8bbc453422c7ce3f678bc6932a6ebe7640051dd49c972d65ed65baa01ab3eb1b",
    "build/wallpaper.spw": "84aeb9472a35c1d8846120c2a5e646dae30525bb66ad75e0bbae8616ba042fef",
    "assets/phipia/media-editor.png": "c5d706b274132b5fcaf0bb016d0da56ddd1dc54b417709364874ad1a58611eb5",
    "build/media-editor-icon.srl": "0e30ba0bfd43ee19ecbef903ae2bf05cab4c0f1e9e28e0c9233907ff6f3bcddd",
    "assets/settings-icon-dock.png": "29bbd3bf688a4eb7ae7c67ef8b9bdbe7c8d101f6694485c585d74e6cd36bbc7f",
    "build/settings-icon.srl": "646fcc71ffb1e621d6a2e828405a1dfcea9930aff50062755fb34698d2c813ba",
    "assets/settings-icon.png": "858e9fc54d2760e7fd486c992f9660831ceb8258b814e5d600e7145f679d9f6b",
    "assets/files-icon-dock.png": "445211ce12458a02dcd3c6f9be113a07dcb8a91fabc9a07ec3f41fd0e4bf7cc6",
    "build/files-icon.srl": "878dc489855709c5874d431d37ab18f01a45d765de4da94ad5cd19343e00003f",
    "assets/files-icon.png": "d07b23abebd9cb95b72a81b11af658ef746ee17105865aa41654a62f1615b044",
    "assets/terminal-icon-dock.png": "380e645c469b454f5c792f75c5d462c6404e333042c19f8edc0088d1cbc2345e",
    "build/terminal-icon.srl": "68b93675d08a36fae73349fba3a172b2d54cd5ea6a4d9437d14d5aa0fa92388b",
    "assets/terminal-icon.png": "bc333d40ad7a59b620ffb37c136a07ea543ad80ab43eefd3562fa236b0310a34",
    "assets/camera-icon-dock.png": "318c029c4a2c57058a1c4a8a33614420a938a1b59ba7b0c77fc30628b76e00a2",
    "build/camera-icon.srl": "8eb00e5a86e92fb47211123d019df6b03fbc2126065451402abc77308f27c125",
    "assets/camera-icon.png": "378b38b0a96760d71af37069d89c5aaaa045f85e30be9311f64640a17742daea",
    "assets/canvas-icon.png": "b750399f8472ad8e1a2fa04c93732bd773efb63cff6c26c660e9872caf077972",
    "assets/canvas-icon-dock.png": "7456bf5390acb86f2f702e2922a01f2206be3e67ad86805566e2128d0d2020a2",
    "build/canvas-icon.srl": "1fa12b05141928d772e544a5455a83915d9588b7f5646d14760cf9babc428f93",
    "assets/store-icon.png": "152565dee4c85dc07e46194e1a727e08b1b5a6d23e95484c7d2c8db3e5cdc8ce",
    "assets/store-icon-dock.png": "56176a9d9836d243b21af117b98ebb33be84755becb5f5e9138b59d1fdf0f209",
    "build/store-icon.srl": "01d02224282afe4f9c2a2451275362b6a7bde8022968707bd5e75f4d9c495282",
    "assets/store-icons.png": "cddf8c4feadf5f0f117f6e50977ee47fd5140db36b3b69505f0c3eefbe0b1b90",
    "build/store-icons.srl": "fd50e96e94664c7d97fa6ebcb4ac9c3adaf29ca57e4fbe9dd91970621b8897a9",
    "assets/settings-category-icons.png": "48aa43fa5ce7051d3f138d293feeff326d3544e4b56fba2bd63454cfada8d441",
    "build/settings-category-icons.srl": "1a47dca6e6a1ea81ce9f838391f6877bbec7107ee2027a1fffde3e427837f08b",
    "assets/icons/lucide/LICENSE": "b495047bd93a9b06913511076f504daba17d5bbeb3e0650f3bb53a4220329c57",
    "assets/icons/lucide/accessibility.svg": "114a9b6983ee20c43d76556bbb5633085a0e082b19464dfa36f1e27768c8f3ea",
    "assets/icons/lucide/code-xml.svg": "a6d2110227be375be21da7324d3c09262c1bf019d06bdeab356d515342ab7cce",
    "assets/icons/lucide/flask-conical.svg": "a2ce857c159bef4c128a6d9f0b6dcbd6504ecf1a2a670ee220efd14fd032b561",
    "assets/icons/lucide/gamepad-2.svg": "b0671de7afbebdafc4d655d867d9851947170b906fb187e749410039df867509",
    "assets/icons/lucide/house.svg": "28216c479005731ef5e382e7d8627eda2b129b019eb1f3d9de9f5915576d448e",
    "assets/icons/lucide/layout-grid.svg": "b8903f61d09b1d75e55c71158277d26570962f69c2099446a251871e0a2d6678",
    "assets/icons/lucide/monitor-cog.svg": "ad9070756393f141d207256ad5507801158b7bf3a71374e15d5fddd3ae1d07d6",
    "assets/icons/lucide/music-2.svg": "fd3e51e40de1d9c5b1872030467fc1e181592072c3a70854133e060b134c3661",
    "assets/icons/lucide/package-check.svg": "ce6b0e8a3102a9dc0a00b68f613dfd43ca6fa45182bc13786aafc338c1961fcf",
    "assets/icons/lucide/package-open.svg": "261a0676ee3db3e5c64a528ac7f81a4893ddb3dfebd0549562f4ef5033757920",
    "assets/icons/lucide/refresh-cw.svg": "2e10dd403c85a24f163d59fc6151aa21147fe9402e1305dfc8979208caee8944",
    "assets/icons/lucide/settings.svg": "0ae27fd0f81999229e3127ac96c5b32edfea448e291d509e76212b917551d66b",
    "assets/icons/lucide/wrench.svg": "b5f3bf792743edb9e81b543dd1b13ccc26d3afe1d33500a1cd8dee2e052d6197",
    "assets/icons/lucide/circle.svg": "3a991bd47beaf9874fba6fdf87bbba442970a89b5e6aa391558bc8b0a00a0513",
    "assets/icons/lucide/clock-3.svg": "0e097d0b66278965fb53bc14900575d95f5c21b9cca932028bbb33294b56c9e2",
    "assets/icons/lucide/eraser.svg": "838d4355dd49340523c38a22a2d23f1aca72ef42f8eba79f6a0c84bf6e27b929",
    "assets/icons/lucide/hard-drive.svg": "324363c40f54e4ca843de0ad1d023b063b70a26a83bb2e0a9e9f4aad9af37378",
    "assets/icons/lucide/image.svg": "619c9db58c879dce6250376ba67d9c9102ca6dbb3b1efb9294c9f180e22273a5",
    "assets/icons/lucide/info.svg": "bc977a64eb96f3e9c1041ffd09a3fceb70e3e65c02b571f76064062ed31f3cb9",
    "assets/icons/lucide/keyboard.svg": "f509925ca81964ed06251ad4702e989bb81a566ffc74d15c2226b4f07ded67be",
    "assets/icons/lucide/monitor.svg": "d1a443233345724859de8e4fb48968ba91e885e12ddbd710701548bd91b39428",
    "assets/icons/lucide/mouse-pointer-2.svg": "a87128b1d33024c22ab598d1ce313081ce8942efd1a70274215bcc53421b7c7e",
    "assets/icons/lucide/network.svg": "626be730feea1e5ea7539fbf78b4a3fcb927ce5d7dc43364f2216908672ac792",
    "assets/icons/lucide/paintbrush.svg": "b0a958ba9889f89c40df6e1c168b8a7aa20f6309780e795c5bce00a4bc11bff6",
    "assets/icons/lucide/palette.svg": "8e7d3fde990cffd10469ba351f27c50202b057a9bb9af5626322e53fd3abc78c",
    "assets/icons/lucide/panel-bottom.svg": "7792f8b39919a347ba7be86fda059c41a46e38dbe23758f7be6aa4514579703e",
    "assets/icons/lucide/rocket.svg": "2b7887d9a5623f2e5e4a5d32716f93829d61b458e03b6eae33a6df648b9eb855",
    "assets/icons/lucide/search.svg": "283d371c2e433817bb9c0c8310caa6c77fa4177c0f4f1168d9c83b97af7389dc",
    "assets/icons/lucide/slash.svg": "2e8b636164f057c3113b04d36a5bb958f78aff454c4a644642da790ae2addbc8",
    "assets/icons/lucide/square.svg": "bd979354f0ab184b95cecf03eedefe40c2dc65830ac6d7e60017b2b25a354acb",
    "assets/icons/lucide/x.svg": "4a9cdab38fbb96162e7dace28e33f4ca0e49d8963a6162abc3d4691b7d675117",
    "assets/icons/lucide/minus.svg": "a0c743ab6dbf545d8a6e19ef3874f48ede686ce68d25e231bd81f540d97b1f19",
    "assets/icons/lucide/volume-2.svg": "fb404f9c128a0579de67b399177631e5edb3502fdc247ceb30e8b15754b46071",
    "assets/canvas-tools.a8": "b19a01ed2147bb782c684691607e174643e720fa0b0711545e0fdfe52cbaafd0",
    "assets/fonts/inter-ui-atlas.png": "3f620de4ba4e340f0dbf673074acc0178f62cb3afed84d996de578f916c2e119",
    "assets/fonts/inter-ui-metrics.txt": "31c2ac1bfe015e48965ad4a0ca101ae22ea9c0db1629297ccd606c44bc20e261",
    "assets/fonts/Inter-LICENSE.txt": "262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a",
    "assets/fonts/InterVariable.ttf": "4989b125924991b90d05b2d16e0e388c48f7d5bb8b30539bbf9c755278d0ccaf",
    "build/ui-font.suf": "b39b35d4749a5eeaa5d900bd4d6579c6df014d70b47756fce1506634e28e7964",
}

WALLPAPERS = [
    "assets/phipia/wallpaper.png",
    *[f"assets/wallpapers/{number:02d}-{name}.png" for number, name in enumerate((
        "galaxy-stars", "milky-way-lake", "milky-way-reflection",
        "forest-waterfall", "waterfall-valley", "desert-dunes", "aurora",
        "aurora-fjord", "golden-mist-forest", "yosemite-mist",
        "alpine-lake", "tropical-sunset", "ocean-cliffs",
    ), 1)],
]
WALLPAPER_MANIFEST = "ce40bd3445223a0e7cc037a610538023df381155a3d72726b96b649e8a03e0d9"


def digest(path: str) -> str:
    source = Path(path)
    if not source.is_file():
        raise SystemExit(f"missing UI asset: {path}")
    return sha256(source.read_bytes()).hexdigest()


def main() -> None:
    for path, expected in PINNED.items():
        actual = digest(path)
        if actual != expected:
            raise SystemExit(f"UI asset digest mismatch: {path}: {actual}")

    manifest = "".join(f"{digest(path)}  {path}\n" for path in WALLPAPERS)
    actual_manifest = sha256(manifest.encode("utf-8")).hexdigest()
    if actual_manifest != WALLPAPER_MANIFEST:
        raise SystemExit(
            f"wallpaper source manifest digest mismatch: {actual_manifest}"
        )
    print(f"UI asset integrity: {len(PINNED)} pins and "
          f"{len(WALLPAPERS)} wallpaper sources verified")


if __name__ == "__main__":
    main()
