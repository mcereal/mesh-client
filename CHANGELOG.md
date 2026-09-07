## [2.32.1](https://github.com/mcereal/mesh-client/compare/v2.32.0...v2.32.1) (2026-09-07)

### Bug Fixes

* **ui:** draw the clock from a seam, so a screenshot is reproducible ([774d142](https://github.com/mcereal/mesh-client/commit/774d14296d557d799cc3e538995888699b281f19))

## [2.32.0](https://github.com/mcereal/mesh-client/compare/v2.31.0...v2.32.0) (2026-09-07)

### Features

* **ui:** slide between screens, from a route derived off the nav ([d0ee590](https://github.com/mcereal/mesh-client/commit/d0ee590069fd717a93c050c8ccc95e3eb5f59951))

### Bug Fixes

* **ui:** let the tab strip decide a transition's direction, both ways round ([e7ab47b](https://github.com/mcereal/mesh-client/commit/e7ab47b6fb4a91514029bbbce7bf331aaf82d429))

## [2.31.0](https://github.com/mcereal/mesh-client/compare/v2.30.0...v2.31.0) (2026-09-07)

### Features

* **ui:** add the slider, and say which numbers are a scale ([9fcf812](https://github.com/mcereal/mesh-client/commit/9fcf812fe3aa1dc492e32820aabe6c9e687110ec))

### Bug Fixes

* **ui:** place a slider's stops after the fill, and refuse anything under its bottom stop ([aee7119](https://github.com/mcereal/mesh-client/commit/aee71190ffcd9ca6485be7e6499a4e21d1ded503)), closes [#102](https://github.com/mcereal/mesh-client/issues/102)

### Documentation

* outline modular map support roadmap ([8f97046](https://github.com/mcereal/mesh-client/commit/8f97046589053a40228e345f877e96ed3d891d54))

## [2.30.0](https://github.com/mcereal/mesh-client/compare/v2.29.1...v2.30.0) (2026-09-07)

### Features

* **ui:** add the screen progress bar and the banner ([236306a](https://github.com/mcereal/mesh-client/commit/236306a730c67d196aa55ff98afcf9809a65e7d2))

## [2.29.1](https://github.com/mcereal/mesh-client/compare/v2.29.0...v2.29.1) (2026-09-07)

### Performance Improvements

* keep Bluetooth and animated UI updates responsive ([115301f](https://github.com/mcereal/mesh-client/commit/115301f508ddfb808fd8381bd66f7d95a1ebe453))

## [2.29.0](https://github.com/mcereal/mesh-client/compare/v2.28.0...v2.29.0) (2026-09-07)

### Features

* **ui:** add the selection control and the segmented button ([f50b612](https://github.com/mcereal/mesh-client/commit/f50b612ef2f6d4c7fac0282d6f578c59aa18aa75))

### Bug Fixes

* **ui:** do not reserve the plain row's gutter twice, or clamp an unknown choice ([0a1bcd2](https://github.com/mcereal/mesh-client/commit/0a1bcd2a228f9dcb8068a31d4393062b0e4b4843))

## [2.28.0](https://github.com/mcereal/mesh-client/compare/v2.27.0...v2.28.0) (2026-09-07)

### Features

* **ui:** count list windows in steps, not items ([d3516e0](https://github.com/mcereal/mesh-client/commit/d3516e08fa75f0bfd8c74951623492c4e5db3751))

## [2.27.0](https://github.com/mcereal/mesh-client/compare/v2.26.1...v2.27.0) (2026-09-07)

### Features

* **ui:** give cards three weights and a verb, and make Status act ([0af9b19](https://github.com/mcereal/mesh-client/commit/0af9b194aea68fc579b8a0f3d10ca8093191f1e4))

### Bug Fixes

* **ui:** keep the focus ring out of card layout, and the Status verbs append-only ([864eff1](https://github.com/mcereal/mesh-client/commit/864eff1c274a10a9749365aee5b537eeed272ca0)), closes [#96](https://github.com/mcereal/mesh-client/issues/96)

## [2.26.1](https://github.com/mcereal/mesh-client/compare/v2.26.0...v2.26.1) (2026-09-07)

### Bug Fixes

* **app:** initialize owned state before publishing or shutdown ([d70849d](https://github.com/mcereal/mesh-client/commit/d70849d2dc0f6a07652dc8f1004ae3b02a55e540))

### Performance Improvements

* keep BLE sync responsive and cache UI rendering work ([d478e44](https://github.com/mcereal/mesh-client/commit/d478e449a44a9230efa10d2cbc0f4db8c0bf0512))

## [2.26.0](https://github.com/mcereal/mesh-client/compare/v2.25.0...v2.26.0) (2026-09-07)

### Features

* **ui:** add the top app bar, with a trail, a back arrow and a badge ([3bb71e3](https://github.com/mcereal/mesh-client/commit/3bb71e36fc3c4864faba43262edb0948b908ec3c))

## [2.25.0](https://github.com/mcereal/mesh-client/compare/v2.24.0...v2.25.0) (2026-09-07)

### Features

* **ui:** fill the leading and marker slots the component set already had ([043ae0b](https://github.com/mcereal/mesh-client/commit/043ae0bb5e246d624c97e20d549a5548a4278b45))

## [2.24.0](https://github.com/mcereal/mesh-client/compare/v2.23.1...v2.24.0) (2026-09-07)

### Features

* **ui:** draw the UI in a rasterised face instead of a 5x7 bitmap ([c81c2b8](https://github.com/mcereal/mesh-client/commit/c81c2b8933433e4d1361af34ae3d8caa2edeefb3))

### Bug Fixes

* **package:** ship the third-party font licences in the pak ([bf68c03](https://github.com/mcereal/mesh-client/commit/bf68c03639ae331a9498281dbf9904e5299d9589))
* **ui:** carry the ground through the signal slot, and format the generated table ([3c087b5](https://github.com/mcereal/mesh-client/commit/3c087b51258057b1920d3803a09e613216cf91f8))

### Code Refactoring

* **ui:** make a glyph coverage rather than a bitmask ([81bbc7c](https://github.com/mcereal/mesh-client/commit/81bbc7c222d977348a8730d36469d1e77e379321))

## [2.23.1](https://github.com/mcereal/mesh-client/compare/v2.23.0...v2.23.1) (2026-09-07)

### Bug Fixes

* **codex:** run repository setup in the environment ([f2eca2f](https://github.com/mcereal/mesh-client/commit/f2eca2f08bc709e6368df6eb1ea77221f530eadf))

## [2.23.0](https://github.com/mcereal/mesh-client/compare/v2.22.0...v2.23.0) (2026-09-07)

### Features

* **i18n:** add Spanish language support ([dd45eee](https://github.com/mcereal/mesh-client/commit/dd45eee2f5a2ab13f747798ad7fb023f51f044f4))

## [2.22.0](https://github.com/mcereal/mesh-client/compare/v2.21.0...v2.22.0) (2026-09-07)

### Features

* **ui:** give a device's state a capsule instead of a supporting line ([e11cafe](https://github.com/mcereal/mesh-client/commit/e11cafeb58ff01a8b60e4840fc30700d1d6adbc4))

### Documentation

* highlight Pak Store installation ([f3531cd](https://github.com/mcereal/mesh-client/commit/f3531cda3185d0ca14a6dfba8677e7d6fe0d5087))
* **ui:** correct two claims the review caught ([cb0a1fa](https://github.com/mcereal/mesh-client/commit/cb0a1fa5e8cb9e6764e2a3f73524c1cbe38e9a13))
* **ui:** re-audit the component set against the current tree ([6dc99c0](https://github.com/mcereal/mesh-client/commit/6dc99c058a374fcc83f5d45e0a52c7b2dcfd392d))

## [2.21.0](https://github.com/mcereal/mesh-client/compare/v2.20.0...v2.21.0) (2026-09-07)

### Features

* **ui:** give the meter a domain and drawn threshold bands, and lists a signal staircase ([3b43698](https://github.com/mcereal/mesh-client/commit/3b4369851dd4a57d12bdaff27b5148f0b9373af8))

### Bug Fixes

* **ui:** only draw a signal staircase where the reading is the node's own ([0e9ca33](https://github.com/mcereal/mesh-client/commit/0e9ca330e43f32467ee4ca0fe42978ec350df4aa))

## [2.20.0](https://github.com/mcereal/mesh-client/compare/v2.19.0...v2.20.0) (2026-09-07)

### Features

* **ui:** draw the chrome as a navigation bar and an action bar ([9d81590](https://github.com/mcereal/mesh-client/commit/9d815905bfa7b3d3ad56b1ddd94b70d80e190b83))

### Bug Fixes

* **ui:** name the compose sheet's A for the row under the cursor ([fd769e7](https://github.com/mcereal/mesh-client/commit/fd769e76114e1fb399d975ed299ec554b0027f5d))

### Documentation

* correct the unit test count ([3eeeafc](https://github.com/mcereal/mesh-client/commit/3eeeafc8a957845c9cfad2d7a2e4b7b0ac7d9f25))

## [2.19.0](https://github.com/mcereal/mesh-client/compare/v2.18.0...v2.19.0) (2026-09-07)

### Features

* **ui:** add a spacing scale and a type scale ([3f9c769](https://github.com/mcereal/mesh-client/commit/3f9c769516b1b20d7d54667b318fa6826ef209bf))

### Bug Fixes

* **ui:** keep the row a title did not actually cost ([315c95d](https://github.com/mcereal/mesh-client/commit/315c95df0a124ad8b6a65db54bd6295245d5e84d))

## [2.18.0](https://github.com/mcereal/mesh-client/compare/v2.17.0...v2.18.0) (2026-09-07)

### Features

* **ui:** add motion tokens and a list scroll rail ([212cd49](https://github.com/mcereal/mesh-client/commit/212cd49ade047355a94fce6e1c623c5e48760197))

### Documentation

* audit the UI component set against Material 3 ([12a442e](https://github.com/mcereal/mesh-client/commit/12a442ea2bd6254239109bba1741b9ac42214723))
* correct three claims in the component audit ([d109046](https://github.com/mcereal/mesh-client/commit/d109046bcfc804bcb0270974bf60ec7434b59861))

## [2.17.0](https://github.com/mcereal/mesh-client/compare/v2.16.0...v2.17.0) (2026-09-07)

### Features

* **ui:** restructure the palette as six Material colour families ([bed6187](https://github.com/mcereal/mesh-client/commit/bed61877aaff55b7d16a1640ae14ce3005003ab2))

### Bug Fixes

* **ui:** keep theme.h C++-includable and let the Radio card see its warnings ([d380784](https://github.com/mcereal/mesh-client/commit/d38078470fdde154e91b9b81c8072d0dc7397d8d))

## [2.16.0](https://github.com/mcereal/mesh-client/compare/v2.15.1...v2.16.0) (2026-09-07)

### Features

* **ui:** add a meter component and give the updater progress ([bb80b9f](https://github.com/mcereal/mesh-client/commit/bb80b9fd76d9222d0ed8e60a4eb1e214116fe6a3))

### Bug Fixes

* **ui:** measure a card's label column from labels, not row kinds ([9999659](https://github.com/mcereal/mesh-client/commit/9999659206c20da1b24652c4b6b138208f2fede1))

## [2.15.1](https://github.com/mcereal/mesh-client/compare/v2.15.0...v2.15.1) (2026-09-07)

### Bug Fixes

* **ui:** round the icon buffer bound the way the box is rounded ([142902d](https://github.com/mcereal/mesh-client/commit/142902dacb498b0607418b2adc8d068dd63442de))
* **ui:** stop the icon sprites being cropped ([230cc4d](https://github.com/mcereal/mesh-client/commit/230cc4d17f8be8aa79af73dbd67244bffa85b7c8))

## [2.15.0](https://github.com/mcereal/mesh-client/compare/v2.14.0...v2.15.0) (2026-09-07)

### Features

* **ui:** draw the transient notice as a Material snackbar ([740840b](https://github.com/mcereal/mesh-client/commit/740840bb7c5147579a989a060cdcb0798427d3f0))

### Code Refactoring

* **ui:** centre the snackbar on the panel ([d4329eb](https://github.com/mcereal/mesh-client/commit/d4329eb5dcd4d4f08c2c15e3d7557b362660aa74))

## [2.14.0](https://github.com/mcereal/mesh-client/compare/v2.13.0...v2.14.0) (2026-09-07)

### Features

* **ui:** draw the draft box and the confirmation as components ([56befdc](https://github.com/mcereal/mesh-client/commit/56befdcc7d0952916780efb57c52cc2c58526fe4))

### Bug Fixes

* **ui:** stack a dialog's answers when they will not fit side by side ([47a6d53](https://github.com/mcereal/mesh-client/commit/47a6d537d92bd0a59dce2d345c71c88d579a8f07))

## [2.13.0](https://github.com/mcereal/mesh-client/compare/v2.12.2...v2.13.0) (2026-09-07)

### Features

* **ui:** draw row markers as themed Material icons ([3e4f2b4](https://github.com/mcereal/mesh-client/commit/3e4f2b4bd9cec8ff5fc25d6132f978e4e3f4db56))

### Bug Fixes

* **ui:** blend a chip's icon over the bar it sits on ([230b483](https://github.com/mcereal/mesh-client/commit/230b483a71e93783bc96a24541dfd60137224ae6))

## [2.12.2](https://github.com/mcereal/mesh-client/compare/v2.12.1...v2.12.2) (2026-09-07)

### Bug Fixes

* **ui:** resolve a node's disc through the node, and never star ourselves ([954f5da](https://github.com/mcereal/mesh-client/commit/954f5da5499c8d009212ad203726010e0ecf8f37))

### Code Refactoring

* **ui:** draw the node, device, picker and compose lists as list items ([bf6fa74](https://github.com/mcereal/mesh-client/commit/bf6fa7467c96624357239df33ed0e0693c8262b9))

## [2.12.1](https://github.com/mcereal/mesh-client/compare/v2.12.0...v2.12.1) (2026-09-07)

### Bug Fixes

* **ui:** drop a trailing slot that does not fit instead of drawing it ([50f1e64](https://github.com/mcereal/mesh-client/commit/50f1e648afc0dec0e821e5361cbc8780ca65800c))

### Code Refactoring

* **ui:** one list item with slots, replacing four row variants ([ab32a39](https://github.com/mcereal/mesh-client/commit/ab32a3967f4449f680fcbe3e087f95924f8c0261))

## [2.12.0](https://github.com/mcereal/mesh-client/compare/v2.11.0...v2.12.0) (2026-09-06)

### Features

* **ui:** add surface tiers, a shape scale and tonal accents to the theme ([3ad71db](https://github.com/mcereal/mesh-client/commit/3ad71db0020b84fa25b69b6fb6c47a21a671e359))

### Bug Fixes

* **ui:** keep the tab strip inside the panel at every glyph scale ([f6a7898](https://github.com/mcereal/mesh-client/commit/f6a78989edb89a25d042e04145ea15de6f28f588))

## [2.11.0](https://github.com/mcereal/mesh-client/compare/v2.10.1...v2.11.0) (2026-09-06)

### Features

* **ui:** add a reusable card component and rebuild the Status tab on it ([7ee9f78](https://github.com/mcereal/mesh-client/commit/7ee9f78c665b460a9b525de52d356074d47a40b9))

### Bug Fixes

* **ui:** clip a card's trailing note instead of dropping it whole ([9bf865b](https://github.com/mcereal/mesh-client/commit/9bf865ba78e58851196b8b7b20bfa07180f23ef4))

## [2.10.1](https://github.com/mcereal/mesh-client/compare/v2.10.0...v2.10.1) (2026-09-06)

### Bug Fixes

* **ui:** give back a message keyboard a pairing prompt displaced ([2276539](https://github.com/mcereal/mesh-client/commit/2276539d7e52b3efc6e48a804f465e4af8e57c58))
* **ui:** make A quick-reply and Y write in a conversation ([b9aa66b](https://github.com/mcereal/mesh-client/commit/b9aa66b48935805ac5f2fc2d34051301f56719be))

## [2.10.0](https://github.com/mcereal/mesh-client/compare/v2.9.2...v2.10.0) (2026-09-06)

### Features

* **ui:** animated toggle switch component ([0bac397](https://github.com/mcereal/mesh-client/commit/0bac397294c8bae990b2368637f1fe2b32ab3b9e))

### Bug Fixes

* **uicap:** give an animating frame the animation's interval ([2e49e28](https://github.com/mcereal/mesh-client/commit/2e49e28b6c4c757949118072e16fac1f38fb99d6))

### Documentation

* refresh the verified unit test count ([e5b07d5](https://github.com/mcereal/mesh-client/commit/e5b07d516951d1a51238c2d013754b9ac5249f7e))

## [2.9.2](https://github.com/mcereal/mesh-client/compare/v2.9.1...v2.9.2) (2026-09-06)

### Code Refactoring

* **ui:** name the two read-only Settings sections apart ([bee63e8](https://github.com/mcereal/mesh-client/commit/bee63e87ee65a5294cd92f6210f07eb244e90fac))

## [2.9.1](https://github.com/mcereal/mesh-client/compare/v2.9.0...v2.9.1) (2026-09-06)

### Bug Fixes

* **i18n:** three gaps the Codex review found ([79e5bee](https://github.com/mcereal/mesh-client/commit/79e5bee0ca803f044b730a208dccc349cdbefb45))

### Code Refactoring

* **i18n:** route every user-facing string through a catalog ([a6e7b41](https://github.com/mcereal/mesh-client/commit/a6e7b41729c35462cd2d2fa22a6e8907624c3733))

## [2.9.0](https://github.com/mcereal/mesh-client/compare/v2.8.0...v2.9.0) (2026-09-06)

### Features

* **ui:** say which cached nodes the radio has forgotten, and offer to drop them ([05b7ee9](https://github.com/mcereal/mesh-client/commit/05b7ee94a96272bded7847fee1be1adda5d75efe))

### Bug Fixes

* **ui:** count what a forget removes, not what is cached or stale ([9fe968f](https://github.com/mcereal/mesh-client/commit/9fe968f5d5921c8bf547df5e027d7d9b909df14e)), closes [#67](https://github.com/mcereal/mesh-client/issues/67)

## [2.8.0](https://github.com/mcereal/mesh-client/compare/v2.7.0...v2.8.0) (2026-09-06)

### Features

* **ui:** give the conversation list avatars, and X to delete a thread ([5ca9a62](https://github.com/mcereal/mesh-client/commit/5ca9a6265d55f05437b017a667da55318799969f))

## [2.7.0](https://github.com/mcereal/mesh-client/compare/v2.6.0...v2.7.0) (2026-09-06)

### Features

* **ui:** hold the d-pad to scroll a long list ([ca1f703](https://github.com/mcereal/mesh-client/commit/ca1f703cb7a696f933c090c5c69327af5f676ca6))

### Bug Fixes

* **ui:** end a held direction when its device goes away ([1553c99](https://github.com/mcereal/mesh-client/commit/1553c99d7a5f236156c03ce2827c45ba8ce37441))

## [2.6.0](https://github.com/mcereal/mesh-client/compare/v2.5.0...v2.6.0) (2026-09-06)

### Features

* accept the two text ports we dropped, and ask nodes for readings ([845dfcb](https://github.com/mcereal/mesh-client/commit/845dfcb73de3a79cc035a47059fa0371e61546e4))
* keep the node telemetry, packet fields and reactions we were dropping ([911bbe6](https://github.com/mcereal/mesh-client/commit/911bbe607416516fcd4c9149a33c8d21a1d847e9))
* **session:** keep NeighborInfo, and draw the mesh as a graph ([6d1e947](https://github.com/mcereal/mesh-client/commit/6d1e947cfd3a41bd1fd367cabcbb0af4cd72ec67))
* **session:** keep the three FromRadio variants we were dropping ([7f79068](https://github.com/mcereal/mesh-client/commit/7f790681217207702194c3575f84681dde517963))

### Bug Fixes

* four gaps in the batch that Codex review caught ([f857eaa](https://github.com/mcereal/mesh-client/commit/f857eaa9e5f40e1e98357d4bb95126b4be7ca50d))

## [2.5.0](https://github.com/mcereal/mesh-client/compare/v2.4.3...v2.5.0) (2026-09-06)

### Features

* **ui:** pick the theme from Settings ([ef16cc1](https://github.com/mcereal/mesh-client/commit/ef16cc172807b869de6add36963a740e1179ea45))

### Bug Fixes

* **ci:** keep the bypass PAT off disk during dependency setup ([0082b72](https://github.com/mcereal/mesh-client/commit/0082b72500b3398873f3346a4e05efd1ebdf6069))
* **ui:** repaint on the press, and keep an explicit capture scale ([5b9f65d](https://github.com/mcereal/mesh-client/commit/5b9f65d4d70d7c90de02cc36b7a0126290e130b5))

### Documentation

* warn that a skip marker in a PR commit blocks the merge ([cc42f9b](https://github.com/mcereal/mesh-client/commit/cc42f9b28cedf60d6b70fd27a825b85b5109ee5f))

## [2.4.3](https://github.com/mcereal/mesh-client/compare/v2.4.2...v2.4.3) (2026-09-06)

### Bug Fixes

* **ble:** drop the bond and the agent when BlueZ goes away ([3a48077](https://github.com/mcereal/mesh-client/commit/3a48077cb8d01a7ae2f93c7f2f398ff88a404fee))
* **ble:** retry the BlueZ bring-up instead of parking at startup ([51a9710](https://github.com/mcereal/mesh-client/commit/51a97109fa7dd458b4dfdfdca44c9e4bfe9d6508))

## [2.4.2](https://github.com/mcereal/mesh-client/compare/v2.4.1...v2.4.2) (2026-09-06)

### Bug Fixes

* **ui:** draw on-fill text on every filled button and selected bubble ([dd6cbad](https://github.com/mcereal/mesh-client/commit/dd6cbad6f78d4b9b59b94903b3241820580967ee))

### Documentation

* note the theme flag and the new test count ([7664daa](https://github.com/mcereal/mesh-client/commit/7664daaba494ddba9f309961c84371835890e6ff))

### Code Refactoring

* **ui:** make the palette, metrics and font a theme ([6eb30bc](https://github.com/mcereal/mesh-client/commit/6eb30bcf6f8845f56b4650655c0678e41bc5f5f3))

## [2.4.1](https://github.com/mcereal/mesh-client/compare/v2.4.0...v2.4.1) (2026-09-06)

### Performance Improvements

* **devtools:** bound the GIF palette on a high-colour screen ([efb0f25](https://github.com/mcereal/mesh-client/commit/efb0f25b571b2ae230e2f82eff00cadc77f6bac8)), closes [#60](https://github.com/mcereal/mesh-client/issues/60)

## [2.4.0](https://github.com/mcereal/mesh-client/compare/v2.3.2...v2.4.0) (2026-09-06)

### Features

* **ui:** draw the thread as a chat transcript ([e9851e2](https://github.com/mcereal/mesh-client/commit/e9851e214ea3737735d9978c7612b0b132294f84))

### Bug Fixes

* **ui:** name the sender on the first bubble on screen ([c2ff691](https://github.com/mcereal/mesh-client/commit/c2ff6918ad5eec01acb3877b8494ff6d8710c8f0))

## [2.3.2](https://github.com/mcereal/mesh-client/compare/v2.3.1...v2.3.2) (2026-09-05)

### Performance Improvements

* **ui:** pack framebuffer colours once and fill spans ([6a2017f](https://github.com/mcereal/mesh-client/commit/6a2017fa1044b5ab1f3361a905bea4bba493587b))
* **ui:** store framebuffer pixels through memcpy ([cc91314](https://github.com/mcereal/mesh-client/commit/cc91314a0e4aef57365e1a8d6344681cc5419301))

## [2.3.1](https://github.com/mcereal/mesh-client/compare/v2.3.0...v2.3.1) (2026-09-05)

### Code Refactoring

* **ui:** extract a reusable component layer for the fb backend ([f1fcd73](https://github.com/mcereal/mesh-client/commit/f1fcd734935316ae4f3afbd5a5d3ecafca2ed6f9))

## [2.3.0](https://github.com/mcereal/mesh-client/compare/v2.2.1...v2.3.0) (2026-09-05)

### Features

* **settings:** add the three large modules (roadmap phase 11) ([01ab213](https://github.com/mcereal/mesh-client/commit/01ab2135ac9c30463ad6a3554c9aaa0a29d08dd2))

### Bug Fixes

* **settings:** reach every GPIO, and let zero_label mean what it says ([36c03bd](https://github.com/mcereal/mesh-client/commit/36c03bdc08bb92b98d79cd2a3450e73daec61dcc))

## [2.2.1](https://github.com/mcereal/mesh-client/compare/v2.2.0...v2.2.1) (2026-09-05)

### Code Refactoring

* **settings:** give the modules one table instead of four lists ([fda8407](https://github.com/mcereal/mesh-client/commit/fda84074fa1ff194250f2e41768fcb8735f96b60))

## [2.2.0](https://github.com/mcereal/mesh-client/compare/v2.1.0...v2.2.0) (2026-09-05)

### Features

* **settings:** add the six small modules (roadmap phase 10) ([e7710bd](https://github.com/mcereal/mesh-client/commit/e7710bd06dd3867afd08309b11fceb8a0ce1727b))

### Bug Fixes

* **settings:** count the new module sections as loaded ([d6c2296](https://github.com/mcereal/mesh-client/commit/d6c229650c94e651a9224377091f9cbc7898a367))

## [2.1.0](https://github.com/mcereal/mesh-client/compare/v2.0.3...v2.1.0) (2026-09-05)

### Features

* **settings:** put the modules behind their own list and complete the three shipped ones ([22041a6](https://github.com/mcereal/mesh-client/commit/22041a6aee1ca5df76ef885756d56e1778428d5c))

### Bug Fixes

* **settings:** expose the air-quality screen toggle and label the Modules footer ([eb6cbc2](https://github.com/mcereal/mesh-client/commit/eb6cbc2988767fe1e72f0c2cba46c22911d6c4ff)), closes [#53](https://github.com/mcereal/mesh-client/issues/53)

### Documentation

* **settings:** plan phases 9-12 for the remaining ModuleConfig sections ([cc11797](https://github.com/mcereal/mesh-client/commit/cc11797effbea2012732127f41816b4bbcc7a8bb))

## [2.0.3](https://github.com/mcereal/mesh-client/compare/v2.0.2...v2.0.3) (2026-09-05)

### Bug Fixes

* **session:** keep the node roster across syncs and name userless nodes ([03dfdb9](https://github.com/mcereal/mesh-client/commit/03dfdb9c2de49c8b77f69b86cd0265c98f7124b6))
* **session:** keep the roster's owning radio across a restart ([8551158](https://github.com/mcereal/mesh-client/commit/85511584421aead9ba0537113e30a6307e51109c))

## [2.0.2](https://github.com/mcereal/mesh-client/compare/v2.0.1...v2.0.2) (2026-09-05)

### Documentation

* point the file maps at the split layout ([120e360](https://github.com/mcereal/mesh-client/commit/120e360c6a5c50b2aa664a37fbfca869c4e0fa88))

### Code Refactoring

* **core:** split app.c into four files along its existing seams ([f9e34e7](https://github.com/mcereal/mesh-client/commit/f9e34e7e78b11f0c67e99fd01b6d7ffe5ba4486a))
* **ui:** split fb.c into draw, screens and backend layers ([28a5de1](https://github.com/mcereal/mesh-client/commit/28a5de16959076208e59af7c87b7828545779285))
* **ui:** split nav.c into five files, one per subject ([bfb2a15](https://github.com/mcereal/mesh-client/commit/bfb2a15c83c04f94dd3f20dcaebea651ff24617c))
* **ui:** split settings.c into model, codec and rows ([bcedef1](https://github.com/mcereal/mesh-client/commit/bcedef1e5d481cb2713ab53cced28fa272ec0ae7))
* **utils:** fold four duplicated primitives into shared helpers ([61aec30](https://github.com/mcereal/mesh-client/commit/61aec3020fbf407f3b37d27d75df53e0d1a453b9))

## [2.0.1](https://github.com/mcereal/mesh-client/compare/v2.0.0...v2.0.1) (2026-09-05)

### Code Refactoring

* drop the MinUI backend, dead code, and mirror include/ to src/ ([b384080](https://github.com/mcereal/mesh-client/commit/b38408012e35e6c58696360f71d3f9f62c6c80ae))

## [2.0.0](https://github.com/mcereal/mesh-client/compare/v1.19.0...v2.0.0) (2026-09-05)

### ⚠ BREAKING CHANGES

* **pak:** lead the entry regardless of its type. The footer's own
wording is preferred over the subject, with its wrapped lines rejoined:
bodies here wrap at 72 characters, so reading one line would cut the
description mid-sentence.

Found by Codex review on #48.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GodvRJ1uaskwwdQYuiPmpU

### Bug Fixes

* **pak:** read breaking-change footers into the store changelog ([f7a787a](https://github.com/mcereal/mesh-client/commit/f7a787a98cee45ea513bb535388d3374b69b7dd5))

## [1.19.0](https://github.com/mcereal/mesh-client/compare/v1.18.0...v1.19.0) (2026-09-05)

### Features

* **settings:** reboot, shut down and reset the radio ([d108784](https://github.com/mcereal/mesh-client/commit/d10878465bad64d49aa2d697e1b6bc77a9ac72df))
* **settings:** set the radio's position, mute and remove nodes ([f9f0a84](https://github.com/mcereal/mesh-client/commit/f9f0a84e23edf1d6b2841e1a50bee0f2de68d86e))

### Bug Fixes

* **settings:** keep both classes of Position edit, and mute once per press ([b08dcd7](https://github.com/mcereal/mesh-client/commit/b08dcd7035c3ca2c93b2350cdfa807a59911d388))

### Documentation

* correct USB auto-connect and Devices tab, split make goals ([92cd4aa](https://github.com/mcereal/mesh-client/commit/92cd4aa59bcd3ae38385196cade25a8e783b6356)), closes [#46](https://github.com/mcereal/mesh-client/issues/46)
* restructure CLAUDE.md and README, consolidate docs/ ([a1b8f33](https://github.com/mcereal/mesh-client/commit/a1b8f331c36fb93138e4339e367d9f381aa1bf9c))

## [1.18.0](https://github.com/mcereal/mesh-client/compare/v1.17.0...v1.18.0) (2026-09-05)

### Features

* **nodes:** ask a node for its name, or have the radio ignore it ([e84c5c7](https://github.com/mcereal/mesh-client/commit/e84c5c7162175a35ba756e7e54481976308225f8))
* **nodes:** trace the route to a node ([df24f3c](https://github.com/mcereal/mesh-client/commit/df24f3c9e4012c6c2154e7349078c1f76a75a9cf))
* **settings:** make the Device, Position and Power sections editable ([4136712](https://github.com/mcereal/mesh-client/commit/41367123e816a299ea0c2a64a387ddea3b6f582f))
* **settings:** make the MQTT section editable ([55b6a60](https://github.com/mcereal/mesh-client/commit/55b6a60705570003eb679deca5e7db189620bd07))
* **status:** show the mesh health the radio already reports ([01619e9](https://github.com/mcereal/mesh-client/commit/01619e960f02b1b2166d8cfcd2b089292a52d641))

### Bug Fixes

* **nodes:** never send a placeholder User when asking a node for its name ([ea2afbf](https://github.com/mcereal/mesh-client/commit/ea2afbf8e0bf40dc0ef9a828f249895cd5d944b5))
* **settings:** mask the MQTT password in its row ([0c8c7c2](https://github.com/mcereal/mesh-client/commit/0c8c7c263c29af1b5d0db8ecc5520f8daeb50629))
* **status:** take the radio's uptime from LocalStats too ([40d878a](https://github.com/mcereal/mesh-client/commit/40d878af636c5efae58f2456b251e8588245b22a))

## [1.17.0](https://github.com/mcereal/mesh-client/compare/v1.16.0...v1.17.0) (2026-09-05)

### Features

* **update:** pick an update channel and install from the About screen ([9ee221e](https://github.com/mcereal/mesh-client/commit/9ee221e85c73bd28bb160a7367c70c01474c0d5c))

### Bug Fixes

* **build:** report the version project(VERSION) actually says ([93260f8](https://github.com/mcereal/mesh-client/commit/93260f8154aa2adad2cbdef477bfc1ab55737497))
* **update:** name the channel Automatic actually resolves to ([5c308bb](https://github.com/mcereal/mesh-client/commit/5c308bbbaf8b77f285694cf3803195933a4fe64e))

## [1.16.0](https://github.com/mcereal/mesh-client/compare/v1.15.0...v1.16.0) (2026-09-05)

### Features

* **ble:** pair PIN-mode nodes and manage the link from the Devices tab ([a7176ad](https://github.com/mcereal/mesh-client/commit/a7176ad317f139e6bf80e37ccbc43b38f05c3e1a))

### Bug Fixes

* **ble:** address review findings on the pairing path ([af9d0a7](https://github.com/mcereal/mesh-client/commit/af9d0a7b1203452fe32f1b79c8258da111da4c2f))
* **message:** say why a message failed instead of just marking it failed ([65605c1](https://github.com/mcereal/mesh-client/commit/65605c18f71615d0cc7f7532e3c2f0034d197bef))

## [1.15.0](https://github.com/mcereal/mesh-client/compare/v1.14.0...v1.15.0) (2026-09-05)

### Features

* **device:** screenshot the Brick's screen with make deploy-shot ([89dc6ea](https://github.com/mcereal/mesh-client/commit/89dc6ea27bfa771c2ac3c826a03bd5f2c293d120))
* **pak:** add Pak Store screenshots taken off the device ([8ba789e](https://github.com/mcereal/mesh-client/commit/8ba789e56df07709c88c7891e580902f67115ab8))
* **pak:** publish MeshClient through the NextUI Pak Store ([e5069e4](https://github.com/mcereal/mesh-client/commit/e5069e420f52489ab5194fa373caf60fe02278c8))

### Bug Fixes

* **update:** ship a CA bundle so self-update works on the Brick ([a06d1cb](https://github.com/mcereal/mesh-client/commit/a06d1cb68acc82022b0ed9a0c821971ec8b7b545))

## [1.14.0](https://github.com/mcereal/mesh-client/compare/v1.13.0...v1.14.0) (2026-09-04)

### Features

* **nodes:** keep your other radios near the top of the Nodes tab ([1cff90d](https://github.com/mcereal/mesh-client/commit/1cff90db4f3d74bb3934cef5594150342f4f9d8c))

## [1.13.0](https://github.com/mcereal/mesh-client/compare/v1.12.0...v1.13.0) (2026-09-04)

### Features

* **update:** show the client version and let it update itself ([487613b](https://github.com/mcereal/mesh-client/commit/487613bdcf80a9dd3d82744e1a53531ca7ac8cda))

### Bug Fixes

* **update:** keep prereleases building and stop dev builds self-updating ([6958fed](https://github.com/mcereal/mesh-client/commit/6958fed87f2059a25a64bb8db2e12060774c7d5b)), closes [#39](https://github.com/mcereal/mesh-client/issues/39)

## [1.12.0](https://github.com/mcereal/mesh-client/compare/v1.11.0...v1.12.0) (2026-09-04)

### Features

* **ui:** show node details and let a node be pinned to the top ([4ec7801](https://github.com/mcereal/mesh-client/commit/4ec7801fe5cfe1788e7cab05e97f8c3e7cdf5998))

## [1.11.0](https://github.com/mcereal/mesh-client/compare/v1.10.0...v1.11.0) (2026-09-04)

### Features

* **ui:** draw emoji and accented node names instead of question marks ([e6501fb](https://github.com/mcereal/mesh-client/commit/e6501fb08a86193402cc9a1d653244ecd8a6766d))

## [1.10.0](https://github.com/mcereal/mesh-client/compare/v1.9.0...v1.10.0) (2026-09-04)

### Features

* **settings:** set the node's clock on connect and edit its time zone ([a5a6086](https://github.com/mcereal/mesh-client/commit/a5a60865dc858048718068b58e14ed12f8620901))

### Bug Fixes

* **ui:** map the Brick's X and Y buttons to the codes it reports ([dbb31b3](https://github.com/mcereal/mesh-client/commit/dbb31b3356b5ee58e621bcf2164b7ba9c4bdd089))
* **ui:** say how many edits a settings refresh kept ([07b8b1a](https://github.com/mcereal/mesh-client/commit/07b8b1a0b66ba54f95f19b40bb97dc9606b04432))

## [1.9.0](https://github.com/mcereal/mesh-client/compare/v1.8.0...v1.9.0) (2026-09-04)

### Features

* **ui:** rebuild Messages around a conversation list ([af0fa31](https://github.com/mcereal/mesh-client/commit/af0fa319dfd5f1ef640bc38d9fbc18d8bd225d0e))

## [1.8.0](https://github.com/mcereal/mesh-client/compare/v1.7.1...v1.8.0) (2026-09-04)

### Features

* **serial:** add the USB serial transport ([a27de5b](https://github.com/mcereal/mesh-client/commit/a27de5b1e1e5df0f36711bb37016a91472c4af70))
* **ui:** make the USB node selectable and preferred ([b068282](https://github.com/mcereal/mesh-client/commit/b06828265c4c5f818b025bf0eedff644e96d7f45))
* **ui:** say why a connect failed ([34e49f3](https://github.com/mcereal/mesh-client/commit/34e49f35509411cf046d58fdc22a3ef51bab1e3d))

### Bug Fixes

* **app:** back auto-connect off when a link fails after connect returns ([f32e901](https://github.com/mcereal/mesh-client/commit/f32e90178f2a3d3650ef8b3e59ae6b449471182b))
* **serial:** read with VMIN=1 so an empty tty is not read as EOF ([6cb6c1e](https://github.com/mcereal/mesh-client/commit/6cb6c1ed2e6ade5c8d50266f26e678c18d344e8d))

## [1.7.1](https://github.com/mcereal/mesh-client/compare/v1.7.0...v1.7.1) (2026-09-04)

### Code Refactoring

* **session:** lift the Meshtastic session out of the BLE transport ([57e7e2b](https://github.com/mcereal/mesh-client/commit/57e7e2b0766c49c7bdda5aae1c88eb14a7d2fe27))

## [1.7.0](https://github.com/mcereal/mesh-client/compare/v1.6.0...v1.7.0) (2026-09-04)

### Features

* **settings:** edit LoRa and Security, with key backup and regeneration ([94270f2](https://github.com/mcereal/mesh-client/commit/94270f25ae858f0aa95a668a9f524633756531ed))

## [1.6.0](https://github.com/mcereal/mesh-client/compare/v1.5.0...v1.6.0) (2026-09-04)

### Features

* **settings:** edit channels and Bluetooth behind a confirm screen ([8a7f0b1](https://github.com/mcereal/mesh-client/commit/8a7f0b1cd5d508357dfce932fcdd1d23e2dff4a1))
* **settings:** list empty channel slots so channels can be added and removed ([7eed2dd](https://github.com/mcereal/mesh-client/commit/7eed2dd5bf0e21caed8b87390006aecf6f41cff1))

## [1.5.0](https://github.com/mcereal/mesh-client/compare/v1.4.0...v1.5.0) (2026-09-04)

### Features

* **settings:** edit and save User, Display, Store & Forward and Telemetry ([1efa5e1](https://github.com/mcereal/mesh-client/commit/1efa5e1de1b13fad6659242e01b81882dddbd723))

## [1.4.0](https://github.com/mcereal/mesh-client/compare/v1.3.0...v1.4.0) (2026-09-04)

### Features

* **settings:** read the radio's configuration over the admin protocol ([bd3666c](https://github.com/mcereal/mesh-client/commit/bd3666c1b9936891ba0cdb9e90ea7392e0f6fb42))

## [1.3.0](https://github.com/mcereal/mesh-client/compare/v1.2.1...v1.3.0) (2026-09-04)

### Features

* **ui:** conversations, channel selection and an on-screen keyboard ([d867140](https://github.com/mcereal/mesh-client/commit/d867140da905b4b6c28a94a20fc1720db0336280))
* **ui:** navigate the HUD with the d-pad and send quick replies ([0e4b4ba](https://github.com/mcereal/mesh-client/commit/0e4b4ba047f0fbcde5d2cc0bc5d1d5d9a61a9c4d))

### Bug Fixes

* **ble:** keep the node you are talking to on the list ([f82485f](https://github.com/mcereal/mesh-client/commit/f82485f6c82ea47f1130b5c3e247ccd58bdf9941))
* **ble:** notice a dropped link, fail unsent messages, and pick targets from a list ([ef4a4c8](https://github.com/mcereal/mesh-client/commit/ef4a4c85bf23d7db633b6244c823421453b2b4dd))
* **ui:** rank message peers above MQTT-fed nodes so they stay listable ([690a592](https://github.com/mcereal/mesh-client/commit/690a592fc7ac79a34285fc295c3e7a7e6a660e8c))

## [1.2.1](https://github.com/mcereal/mesh-client/compare/v1.2.0...v1.2.1) (2026-09-04)

### Bug Fixes

* **ble:** keep the UI live while BlueZ connects ([c3254c1](https://github.com/mcereal/mesh-client/commit/c3254c14487cff754a751e275fadb3f41c5305bc))
* **ble:** wait for GATT service discovery and auto-connect on device ([6953063](https://github.com/mcereal/mesh-client/commit/695306357048967d0d56e554e71c5b83b70f873e))
* **ui:** pan the framebuffer to the page being drawn ([adf0e9f](https://github.com/mcereal/mesh-client/commit/adf0e9ffcb62f6bff4385ebd91571e10a406720a))
* **ui:** write opaque alpha so the HUD is visible on the Brick ([a7b4ebb](https://github.com/mcereal/mesh-client/commit/a7b4ebb6823db2dcd97aac5b364f761e953a6627))

## [1.2.0](https://github.com/mcereal/mesh-client/compare/v1.1.16...v1.2.0) (2026-09-03)

### Features

* **message:** send and receive Meshtastic text messages ([f2e4b58](https://github.com/mcereal/mesh-client/commit/f2e4b583fe00cacd18c343b698bcf006c1bff84d))

### Bug Fixes

* **message:** preserve the cached inbox and reject malformed UTF-8 ([3731a62](https://github.com/mcereal/mesh-client/commit/3731a62d85c692bcb09d17a59f9fb7c20d6381f6))

## [1.1.16](https://github.com/mcereal/mesh-client/compare/v1.1.15...v1.1.16) (2026-09-03)

### Bug Fixes

* **build:** fall through to the next libmsettings candidate on rejection ([025f1e2](https://github.com/mcereal/mesh-client/commit/025f1e2a6775551b9dd8d740f3d24e30ea2902cb))
* **build:** never stage host-arch helpers into the device pak tree ([99fde62](https://github.com/mcereal/mesh-client/commit/99fde623d4416c3a0b73ab41ca2aaaaf9c2c2ab0))

## [1.1.15](https://github.com/mcereal/mesh-client/compare/v1.1.14...v1.1.15) (2026-09-03)

### Bug Fixes

* **ui:** paint an initial frame and add a way to quit from the device ([8fe9bc8](https://github.com/mcereal/mesh-client/commit/8fe9bc81794403eb22336f9fb4e319a1a659e173))
* **ui:** print transport-only updates and harden the setup script ([712f3d9](https://github.com/mcereal/mesh-client/commit/712f3d9ffc71025260e0d6d4959e6e91b18a5d92))

## [1.1.14](https://github.com/mcereal/mesh-client/compare/v1.1.13...v1.1.14) (2026-09-03)

### Bug Fixes

* **ble:** yield between FromRadio reads and retry failed drains ([3ee5ed3](https://github.com/mcereal/mesh-client/commit/3ee5ed30bd4c95253a8181d2aec98c2233a14d24))
* speak the Meshtastic BLE GATT protocol instead of Nordic UART ([1fe2933](https://github.com/mcereal/mesh-client/commit/1fe29333d0d84ee2422083074758aa493e96d56a))

## [1.1.13](https://github.com/mcereal/mesh-client/compare/v1.1.12...v1.1.13) (2026-09-03)


### Bug Fixes

* remove orphaned SDL backend and link MinUI helpers statically ([3afd87d](https://github.com/mcereal/mesh-client/commit/3afd87dbc8c4b0268d3146ae8c6c384238e84e4a))


### Documentation

* sync docs with current code and add CLAUDE.md ([582674c](https://github.com/mcereal/mesh-client/commit/582674c14db9933d700d476b724ab9e83d062d92))

## [1.1.12](https://github.com/mcereal/mesh-client/compare/v1.1.11...v1.1.12) (2025-10-03)


### Bug Fixes

* framebuffer backend ([b62e931](https://github.com/mcereal/mesh-client/commit/b62e9313d91ddbec76d306412ed9d99d3d3f9240))

## [1.1.11](https://github.com/mcereal/mesh-client/compare/v1.1.10...v1.1.11) (2025-10-03)


### Bug Fixes

* implement SDL backend ([62fcb09](https://github.com/mcereal/mesh-client/commit/62fcb09f2d09855b0ea32a3a49c53edfd805c3f4))

## [1.1.10](https://github.com/mcereal/mesh-client/compare/v1.1.9...v1.1.10) (2025-10-03)


### Bug Fixes

* synchronous run printy to tty ([5976df9](https://github.com/mcereal/mesh-client/commit/5976df983b92640d1a0e137b5bda249cb15069bf))

## [1.1.9](https://github.com/mcereal/mesh-client/compare/v1.1.8...v1.1.9) (2025-10-03)


### Bug Fixes

* cli write to tty ([92668b9](https://github.com/mcereal/mesh-client/commit/92668b984c1d79db74f9f005fcffbf50a0d5d8ee))

## [1.1.8](https://github.com/mcereal/mesh-client/compare/v1.1.7...v1.1.8) (2025-10-03)


### Bug Fixes

* mirror output to tty ([800a606](https://github.com/mcereal/mesh-client/commit/800a6062efdcad0463635a5b63243aed1c74b5ed))

## [1.1.7](https://github.com/mcereal/mesh-client/compare/v1.1.6...v1.1.7) (2025-10-03)


### Bug Fixes

* reset launcher to simple synchronous run ([c8f95ce](https://github.com/mcereal/mesh-client/commit/c8f95ce008b117ea28d6c7ea37330233694c6461))

## [1.1.6](https://github.com/mcereal/mesh-client/compare/v1.1.5...v1.1.6) (2025-10-03)


### Bug Fixes

* tee to log file and run meshclient synchronbously ([2fb2381](https://github.com/mcereal/mesh-client/commit/2fb23815b51565ef36d7b5f0cb4e2f0a7078babb))

## [1.1.5](https://github.com/mcereal/mesh-client/compare/v1.1.4...v1.1.5) (2025-10-03)


### Bug Fixes

* create FIFO in pak userdata and run meshclient in the background ([e7ad1e2](https://github.com/mcereal/mesh-client/commit/e7ad1e229eca631fc5fe2d1427ffc8ae3c1e8332))

## [1.1.4](https://github.com/mcereal/mesh-client/compare/v1.1.3...v1.1.4) (2025-10-03)


### Bug Fixes

* set meshclient backend cli ([0637201](https://github.com/mcereal/mesh-client/commit/06372019dde25b7df58e7ed04d8dc70047c0e20d))

## [1.1.3](https://github.com/mcereal/mesh-client/compare/v1.1.2...v1.1.3) (2025-10-03)


### Bug Fixes

* helper directory path ([6f6aa10](https://github.com/mcereal/mesh-client/commit/6f6aa10754add28d9986c8057ab37c1696e648c7))

## [1.1.2](https://github.com/mcereal/mesh-client/compare/v1.1.1...v1.1.2) (2025-10-03)


### Bug Fixes

* trigger rebuild with helper paths ([38b6ecc](https://github.com/mcereal/mesh-client/commit/38b6ecc78fc2f7afd47155ce8e94b66e7fc973cd))

## [1.1.1](https://github.com/mcereal/mesh-client/compare/v1.1.0...v1.1.1) (2025-10-03)


### Bug Fixes

* cache toasting ([5ec62f3](https://github.com/mcereal/mesh-client/commit/5ec62f36ff533c2fca8a6b82db56b7dd3882083b))
* dbus system socket ([57091f9](https://github.com/mcereal/mesh-client/commit/57091f9ad8d57526658734653a7ef89cf3930d92))

## [1.1.0](https://github.com/mcereal/mesh-client/compare/v1.0.6...v1.1.0) (2025-10-03)


### Features

* build libdbus from source for static linking to enable BLE support ([c760b64](https://github.com/mcereal/mesh-client/commit/c760b64fdc8168664a6e78837248af9823fa8a03))


### Bug Fixes

* add dbus include and library paths to cmake build flags ([9baf7fb](https://github.com/mcereal/mesh-client/commit/9baf7fb328bf04a2aabf381c6cb4a740563b6080))
* build expat library before libdbus to satisfy dependencies ([833fdce](https://github.com/mcereal/mesh-client/commit/833fdcec72aa6de1728c55421491f219ee629811))
* explicitly set cross-compiler tools for libdbus configure ([c79753e](https://github.com/mcereal/mesh-client/commit/c79753e5f8444b545a257aedfd35d8bfa60bfbca))

## [1.0.6](https://github.com/mcereal/mesh-client/compare/v1.0.5...v1.0.6) (2025-10-03)


### Bug Fixes

* add --foreground flag to launch.sh to keep app running ([54db334](https://github.com/mcereal/mesh-client/commit/54db33465b1286a7c06b6ad2f9c70b6b4b01ae99))

## [1.0.5](https://github.com/mcereal/mesh-client/compare/v1.0.4...v1.0.5) (2025-10-03)


### Bug Fixes

* force cmake to use system python3 with protobuf packages installed ([814f71a](https://github.com/mcereal/mesh-client/commit/814f71a6c6f10c7a8475bcaefff0d17b68e70deb))
* install python protobuf packages before toolchain setup ([d2c40ee](https://github.com/mcereal/mesh-client/commit/d2c40ee2c9cb2c46cdf7a4281d3dc8a9b15864d9))
* repair corrupted YAML in semantic-release workflow ([1b0db55](https://github.com/mcereal/mesh-client/commit/1b0db554d5a5a63a05896aae137609a51532f668))
* switch to ubuntu-20.04 with glibc 2.31 for better TrimUI device compatibility ([8377a66](https://github.com/mcereal/mesh-client/commit/8377a66208d0dd0862dcd1b920929caf81fa6552))
* trigger rebuild with ubuntu-20.04 runner ([ee211fc](https://github.com/mcereal/mesh-client/commit/ee211fc56ed9de12640d8c50bbc4a468e78f726e))
* use bootlin musl toolchain instead of musl.cc for reliable downloads ([279b238](https://github.com/mcereal/mesh-client/commit/279b238cbcca23da9ebfa4767bb1ec08ef7470a5))
* use musl-libc cross-compiler for fully static ARM binary with no glibc dependency ([88fbe76](https://github.com/mcereal/mesh-client/commit/88fbe765a9596c6bb2bea340dcb278371d33741e))

## [1.0.4](https://github.com/mcereal/mesh-client/compare/v1.0.3...v1.0.4) (2025-10-03)


### Bug Fixes

* use static linking for libgcc/libstdc++ to avoid glibc version conflicts on TrimUI ([83c8136](https://github.com/mcereal/mesh-client/commit/83c81369751a6a97c71d0721a869d3653d075534))

## [1.0.3](https://github.com/mcereal/mesh-client/compare/v1.0.2...v1.0.3) (2025-10-03)


### Bug Fixes

* add ARM cross-compilation to semantic-release workflow for TrimUI Brick ([9838a21](https://github.com/mcereal/mesh-client/commit/9838a219adee0b0042050e1eb658d7387dbf4c9c))
* disable pkg-config for cross-compilation to skip dbus dependency check ([3fd0fdc](https://github.com/mcereal/mesh-client/commit/3fd0fdc9437f3a4bc8244231757ce8abf508def7))
* install ARM dbus libraries and configure cmake for proper cross-compilation ([dc6de56](https://github.com/mcereal/mesh-client/commit/dc6de56a239173584cff413e4fc15b40f6135fb2))

## [1.0.2](https://github.com/mcereal/mesh-client/compare/v1.0.1...v1.0.2) (2025-10-03)


### Bug Fixes

* bump minor to trigger build ([478ef6c](https://github.com/mcereal/mesh-client/commit/478ef6ca90a06d3fdf8c1ebcf45ca6004e1fad34))

## [1.0.1](https://github.com/mcereal/mesh-client/compare/v1.0.0...v1.0.1) (2025-10-03)


### Bug Fixes

* handshake snapshots restored on launch ([4d7b1c6](https://github.com/mcereal/mesh-client/commit/4d7b1c68ae1eafc6cf01a9fbb86280c3a8b022a3))

## 1.0.0 (2025-10-03)


### Bug Fixes

* add required python packaging ([f850fd6](https://github.com/mcereal/mesh-client/commit/f850fd6cf6599b8882e14e1d45e36c112f26e107))
* restore missing tools dir packaging script and add ble connection scaffolding ([b04634a](https://github.com/mcereal/mesh-client/commit/b04634abbe6e138ced7fa49e7fe0244f032583dc))

# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
