## [2.75.0](https://github.com/mcereal/mesh-client/compare/v2.74.0...v2.75.0) (2026-10-01)

### Features

* **build:** run on the Miyoo Mini Plus under Onion OS ([14beb48](https://github.com/mcereal/mesh-client/commit/14beb489daaad806b978a68c8eebc4bcc902d54f))
* **crash:** send Windows reports with PE debug metadata ([faf6423](https://github.com/mcereal/mesh-client/commit/faf64230fbce7a675d5bba956faa5214fb1d70a7))
* **meshcore:** choose which heard nodes are added, and say when the list is full ([531101e](https://github.com/mcereal/mesh-client/commit/531101e63ca5d3ec4f20b6f6fbe44e28cea8760c))
* **meshcore:** read all 40 of a companion's channel slots, not the first eight ([78b3256](https://github.com/mcereal/mesh-client/commit/78b325688dc801c8b20e8ef4f9d427b78fe2e178))
* **meshcore:** say what a server took us as at login ([0c227c8](https://github.com/mcereal/mesh-client/commit/0c227c8b94e41ff0787b454fb95179ea2d201318))
* **meshcore:** share a contact with the nodes nearby ([0e32163](https://github.com/mcereal/mesh-client/commit/0e321637ac51c538418ac51f04cb9d9d3b581180))
* **meshcore:** show a contact's stored route and let the user forget it ([c46f460](https://github.com/mcereal/mesh-client/commit/c46f46094b6788972583e985f088e4918dca5a8c))
* **meshcore:** show the radio's own stats on the Radio tab ([db88c11](https://github.com/mcereal/mesh-client/commit/db88c11eb8440ee51d543e1155d40fa5e5e16115))
* **meshcore:** trace a repeater's route with SNR on every link ([04e5fbc](https://github.com/mcereal/mesh-client/commit/04e5fbceec90e550968371c7976f6002d29fa739))
* **net:** say the clock is wrong when a certificate is not valid yet ([6d4bcd0](https://github.com/mcereal/mesh-client/commit/6d4bcd0a1d6fd608d8b1220a1b6bf98a76e58716))
* **stats:** count answered traceroutes and record the longest ([a58a333](https://github.com/mcereal/mesh-client/commit/a58a33333f6936ce3bdba0bf968088ce30b26fec))
* **stats:** count connections and time connected ([51dc89b](https://github.com/mcereal/mesh-client/commit/51dc89b9ac07d4e0c938dc8b82b18f275660110e))
* **stats:** count nodes heard directly, and record the longest absence ([6594d04](https://github.com/mcereal/mesh-client/commit/6594d046348dfe0c255ba2e869ff1a40f7f9c1c9))
* **stats:** count waypoints, key verifications, shares and S&F recoveries ([d25d29c](https://github.com/mcereal/mesh-client/commit/d25d29c270b80d1b88b44b0d8f2e9a0d1adea0c7))
* **stats:** name the most messaged contact ([edeb791](https://github.com/mcereal/mesh-client/commit/edeb7915893c57d80516f3070e43d089f1a436bc))
* **stats:** record the quietest signal decoded, in dBm ([c707a4a](https://github.com/mcereal/mesh-client/commit/c707a4a360e44542e1619ad39bd14e859bfe2293))
* **stats:** remember when a node was first heard, and who is known longest ([b2f5333](https://github.com/mcereal/mesh-client/commit/b2f53334dfa5ddb33bf4bbf5b4626aaf420b884f))
* **stats:** tally the hardware and roles of the nodes heard ([88e801f](https://github.com/mcereal/mesh-client/commit/88e801f55d5f81079d63d9030b9ce6064b05e936))
* **ui:** a desktop window at a desktop's size, with panes and a board that fill it ([a37d74b](https://github.com/mcereal/mesh-client/commit/a37d74b9e5c6b2a69083d9376d5a8498af6339fa))
* **ui:** a window's pop-up menus, and a Radio tab headed like the others ([9a7c661](https://github.com/mcereal/mesh-client/commit/9a7c661467f84fae2643eb85c36983c3fa7373cd))
* **ui:** give a Mac window a menu bar, and the same chords everywhere else ([d9399e4](https://github.com/mcereal/mesh-client/commit/d9399e437a221de1fd3f3ed760389c09ca2f8895)), closes [mcereal/inkcell#92](https://github.com/mcereal/inkcell/issues/92)
* **ui:** say what a window's reader does, not which Brick button to press ([3a3989c](https://github.com/mcereal/mesh-client/commit/3a3989c5594ceebf94b954b40dd5d2782c7f5d5e))
* **ui:** the Radio tab as a dashboard on a wide window ([50781e0](https://github.com/mcereal/mesh-client/commit/50781e0845410b2be21b614de18c4618d0571fe4)), closes [mcereal/inkcell#89](https://github.com/mcereal/inkcell/issues/89)

### Bug Fixes

* **app:** pick the wording from the backend that opened, not the one chosen ([ddc4ca2](https://github.com/mcereal/mesh-client/commit/ddc4ca2ba3524b59b449930cae4aea337d5aeafd))
* **build:** clear a release's cached version on the next Miyoo dev build ([94145b0](https://github.com/mcereal/mesh-client/commit/94145b05a8420df3151ddc5239030c2d760164c7))
* **build:** export what env.sh sets, and return the client's status ([1b3033e](https://github.com/mcereal/mesh-client/commit/1b3033e7c0177f48332d2dc461397aca14b59416))
* **build:** give the Miyoo build its own update asset, and cut back its log ([4a3db21](https://github.com/mcereal/mesh-client/commit/4a3db21a9f52167dc4bec202a97a3e1c46e418f3))
* **meshcore:** keep a stored route off nodes the radio does not carry ([4190178](https://github.com/mcereal/mesh-client/commit/4190178baa93f8877cad24862495054aafafc92f))
* **meshcore:** land a stats poll whole, and match STATS to the kind asked ([f3c0626](https://github.com/mcereal/mesh-client/commit/f3c0626eaf2135b1620593e6afc15da2e15bffa0))
* **meshcore:** read a trace answer that beats its SENT; fall back on long routes ([2768209](https://github.com/mcereal/mesh-client/commit/2768209c30c654e84064e46fc389eb73e6a12ad2))
* **meshcore:** take AUTOADD_CONFIG only as GET_AUTOADD_CONFIG's answer ([f88c87b](https://github.com/mcereal/mesh-client/commit/f88c87b1b7805a6fc30dceb2ae59316add5f2484))
* **release:** upload Windows debug files to Sentry ([887a11a](https://github.com/mcereal/mesh-client/commit/887a11ae06a6caffd3fa839151a7ffe96020a403))
* **site:** add the previews block wrangler preview requires ([a9f7f67](https://github.com/mcereal/mesh-client/commit/a9f7f675a0d2409501054f918dc144110293fd75))
* **site:** declare preview_urls and observability as the dashboard has them ([827b6c0](https://github.com/mcereal/mesh-client/commit/827b6c00ae0a273457eaf7d22ca800c1495d78bd))
* **stats:** count a link switched between radios, and CLI runs ([9bee2c8](https://github.com/mcereal/mesh-client/commit/9bee2c818548fee4beed5b828ffda191d9e6cb35))
* **stats:** count direct messages beside every node, so most messaged is exact ([47f1c63](https://github.com/mcereal/mesh-client/commit/47f1c632314b70cbb1d02e1d2c7c1a298b8109e3))
* **stats:** head the delivery rows "Direct delivery"; test MeshCore feeds the stats ([f83530e](https://github.com/mcereal/mesh-client/commit/f83530e679ebc04495a40f2dd521a650f33043ba))
* **stats:** keep the reset on a full Stats page; a radio gives up the longest known ([126c71b](https://github.com/mcereal/mesh-client/commit/126c71bc29e3ed99a1c06ed311c11cc1457ae638))
* **stats:** measure an absence only between receiver-side stamps ([e7e2611](https://github.com/mcereal/mesh-client/commit/e7e26118b8d12b5f58eb4bda660507c5e0ab2c3e))
* **stats:** name a record whose radio spoke after it was set ([39d3372](https://github.com/mcereal/mesh-client/commit/39d33724c249b870cb5676b67d160bf24ff2518b))
* **stats:** name only the radio announced on a link ([9024412](https://github.com/mcereal/mesh-client/commit/9024412f626bbde522b30349e85ea668f1ad2aa3))
* **stats:** record a trace's whole length, not the stops it keeps ([a12c74a](https://github.com/mcereal/mesh-client/commit/a12c74a71951720651f1c5169ee5424f19aedd0c))
* **ui:** draw a hashtag channel's name with one '#', not two ([05333bc](https://github.com/mcereal/mesh-client/commit/05333bcc289e51e594bf1ca2c3b4d82642b64b77))
* **ui:** grey out the tab already up, and fill the menu count before the context ([7241207](https://github.com/mcereal/mesh-client/commit/724120740a46a42303a71e9d8490ad7088b65a97)), closes [mcereal/inkcell#92](https://github.com/mcereal/inkcell/issues/92)
* **ui:** keep the node menu's heading through a resize, and its row for the keys ([54455ea](https://github.com/mcereal/mesh-client/commit/54455eab410da049de51453a47fc8864a95219c9))
* **ui:** name a Reach neighbour without a callsign, and never round a failed delivery up to 100% ([8e49e1f](https://github.com/mcereal/mesh-client/commit/8e49e1fc179e7dabcff7320ca6dd816838c17b7c))
* **ui:** offer Find only over a roster, and pin inkcell to its main ([935d3d2](https://github.com/mcereal/mesh-client/commit/935d3d2450eb689dff2893ccd611ef1eb01cff2e)), closes [mcereal/inkcell#92](https://github.com/mcereal/inkcell/issues/92)
* **ui:** offer no menu item while a pop-up in the frame owns input ([61aed5d](https://github.com/mcereal/mesh-client/commit/61aed5def44ef668f92cda134c9a1e78202e50d9)), closes [mcereal/inkcell#92](https://github.com/mcereal/inkcell/issues/92)
* **ui:** open a choice's pop-up on a value its row allows ([a6c684d](https://github.com/mcereal/mesh-client/commit/a6c684d9e92e699df5ecff41a7b837fa6d92e3ee))
* **ui:** re-resolve a choice pop-up's cursor when its row's values change ([76af0f2](https://github.com/mcereal/mesh-client/commit/76af0f26b1858065483f7f4e3b8331807db5e0c3))
* **ui:** window both pop-up menus to the room beside what they hang from ([130e68f](https://github.com/mcereal/mesh-client/commit/130e68fbc217d3befbd3d3880af5d5124b187f94))
* **windows:** close runtime timers through inkwell ([36d41b9](https://github.com/mcereal/mesh-client/commit/36d41b9c309000ee9c20f8a5f027e2625b736008))

### Documentation

* describe Windows symbol upload in architecture ([75d37c0](https://github.com/mcereal/mesh-client/commit/75d37c09ace018f1e5ddebb543e640caa3286e0b))
* **site:** add the user guide at meshclient.dev/guide ([c061a71](https://github.com/mcereal/mesh-client/commit/c061a71eb88f7e8679bafe9c52379145f5e9364f))
* **site:** note what the Windows build cannot do yet ([13516db](https://github.com/mcereal/mesh-client/commit/13516db8d0e38af3dd5c1eae12cf168e7283886b))
* **site:** spell out the Workers Builds root directory ([fc7f1be](https://github.com/mcereal/mesh-client/commit/fc7f1be92557acd3ac0c495bae38f79263310de9))

## [2.74.0](https://github.com/mcereal/mesh-client/compare/v2.73.0...v2.74.0) (2026-09-30)

### Features

* a silent radio's chosen board can take Meshtastic instead of MeshCore ([1605692](https://github.com/mcereal/mesh-client/commit/16056926e6eaf98624072b0865e5772da983eec7))
* **app:** keep the lifetime stats ([693bad4](https://github.com/mcereal/mesh-client/commit/693bad435880bce2b1eba8c027d831a2b33a3ec8))
* **backup:** back up a MeshCore radio, contacts and all ([2c223d8](https://github.com/mcereal/mesh-client/commit/2c223d8f7c929687c8978d50f9822ee51c76edb7))
* **backup:** compare two backups of a radio, and list and remove them on the card ([433e331](https://github.com/mcereal/mesh-client/commit/433e3314314fb5f0c8fe4bdc363ef96e8acf916b))
* **backup:** keep a radio's settings on the card, starting with Meshtastic ([ac48b3a](https://github.com/mcereal/mesh-client/commit/ac48b3a70c116aee64cc562b1e323b3c5ad0fc36))
* **backup:** plan a Meshtastic restore and send it as one edit transaction ([6a5aa73](https://github.com/mcereal/mesh-client/commit/6a5aa73c410a03cf83cdd1cb0b44786141f482e0))
* **backups:** carry a radio's identity key in a backup asked for with it ([a4184a5](https://github.com/mcereal/mesh-client/commit/a4184a5f292f5706b265adda0cd40a6c2731a5be))
* **backups:** restore a MeshCore radio from a backup and check it took ([500960b](https://github.com/mcereal/mesh-client/commit/500960b7909ea31c8caf24abe71df8b80b0c0dce))
* **backups:** restore a MeshCore radio's contacts from a backup ([e6fd5ea](https://github.com/mcereal/mesh-client/commit/e6fd5ea41f82b631bac0a71669108bca23ba2845))
* **backups:** restore a Meshtastic radio from a backup and check it took ([e340447](https://github.com/mcereal/mesh-client/commit/e340447efa76ab16578ecd901a85c9dc4d868621))
* **backup:** take backups on first connect, before writes and firmware, and on request ([0c38aa7](https://github.com/mcereal/mesh-client/commit/0c38aa74ce4ffef94a2fcd1e577abb8aa9c0c429))
* install MeshCore onto a radio that answers nothing on USB ([3558be1](https://github.com/mcereal/mesh-client/commit/3558be147dbe6308d5a541a5f660bd244b792c35))
* **lifetime:** count what the client has seen across every run and radio ([fe07e96](https://github.com/mcereal/mesh-client/commit/fe07e96dd90e5d3164ccd257c9315866784ddbaf))
* **map:** draw every installed pack as one map ([d8f3588](https://github.com/mcereal/mesh-client/commit/d8f3588efa5a10ef462407e6a643f642f6b453e2))
* **map:** draw past a pack's deepest level by enlarging its tiles ([f6b8338](https://github.com/mcereal/mesh-client/commit/f6b8338ab31858b657bb5dcaf4f58331b0bd791d))
* **maps:** download map packs from the catalog, in resumable pieces ([7e9a9a5](https://github.com/mcereal/mesh-client/commit/7e9a9a57fa3d97577a2b3d5d16cb1efa83680b1f))
* **maps:** download, update and delete map packs from Settings > Maps ([99624a7](https://github.com/mcereal/mesh-client/commit/99624a7c43edbf3193cab9f98332c395f1e5f61e))
* **maps:** hold a catalog of every country ([c53bd5e](https://github.com/mcereal/mesh-client/commit/c53bd5e82fc0b685ef4e578d8c7888e33bf40d76))
* **meshcore:** plan a restore of a radio's settings from a backup ([1d36cfe](https://github.com/mcereal/mesh-client/commit/1d36cfef125016d08a2914026d39320207f4c1e4))
* **meshcore:** send a repeater commands and ask it for its neighbours ([0326066](https://github.com/mcereal/mesh-client/commit/032606631eb91d63986ccbf1c1c43612acfe86bf))
* **messages:** name a sender in the tint their avatar wears ([4fe66a3](https://github.com/mcereal/mesh-client/commit/4fe66a304bd0c9bb29eb846ba9ad171eb6627c14))
* move a MeshCore radio on its cable to the Bluetooth build ([0666796](https://github.com/mcereal/mesh-client/commit/0666796d54c73ad1ababe4ea28b0e407c2639a69))
* **nodes:** badge the Nodes tab and announce newly discovered nodes ([e3fcc15](https://github.com/mcereal/mesh-client/commit/e3fcc159a8f18863ed0aa033ebc1e9a42705b471))
* **nodes:** stand a node's facts in two columns where there is room ([6fc3154](https://github.com/mcereal/mesh-client/commit/6fc3154198942dd4aca076899477a1dca1438128))
* pair a radio moved to Bluetooth with the PIN it kept ([1ee9829](https://github.com/mcereal/mesh-client/commit/1ee982968a0c5473e7ab767f26372d190b5d4114))
* **profiles:** make a profile from a backup, apply it, and carry it as a .cfg ([acad52b](https://github.com/mcereal/mesh-client/commit/acad52b6e6286321ec5f534725ba5f0c6ff061ac))
* **profiles:** make, list, compare, apply and share profiles on screen ([2d2dc79](https://github.com/mcereal/mesh-client/commit/2d2dc7931cd7d89c579d2f8e923a9b1fdc31d029))
* send a crash report to Sentry when the user asks ([eae7e59](https://github.com/mcereal/mesh-client/commit/eae7e59b1fec747c814c7fc78886ece71edeefcd))
* **session:** announce messages and nodes as they are recorded ([356d855](https://github.com/mcereal/mesh-client/commit/356d855d633f71bac5c3f93089b243bfdedfd7d9))
* set a MeshCore radio's Bluetooth PIN from Settings ([709cbac](https://github.com/mcereal/mesh-client/commit/709cbac1ccc74fe6984329577939ea0e8cd9fd5b))
* **split:** preview the row under the list's cursor beside it ([0e3a213](https://github.com/mcereal/mesh-client/commit/0e3a213b3579707f4f4f55aa44e82ef4e42a0c52)), closes [mcereal/inkcell#84](https://github.com/mcereal/inkcell/issues/84)
* **stats:** count delivered and undelivered messages ([78813eb](https://github.com/mcereal/mesh-client/commit/78813ebba23709c5844e8cc6a3e532e21aeee996))
* **stats:** count MQTT and private messages, and record the farthest node heard and weakest signal ([1a3caf3](https://github.com/mcereal/mesh-client/commit/1a3caf3e0c8def6b3d4671b870eda8ec6a348a78))
* **stats:** name who set each record, and when ([95e52ed](https://github.com/mcereal/mesh-client/commit/95e52ed3d629d14a1faa5369fd3384bfaa12ed07))
* **status:** show how many nodes this client has ever heard ([cb577e5](https://github.com/mcereal/mesh-client/commit/cb577e59a8ebf658611f3e874e9a0149361d4922))
* **ui:** a MeshCore repeater's thread is its console, and its sheet asks for neighbours ([75a1c0c](https://github.com/mcereal/mesh-client/commit/75a1c0ca6303bdcdf39eb1f8b5b839dcb2e0570e))
* **ui:** a side rail the reader folds to icons or unfolds into labelled tabs ([fb17e67](https://github.com/mcereal/mesh-client/commit/fb17e67538c3a5bc834215a05af148d06bb4d9bf))
* **ui:** a Stats page for the lifetime counts ([f33d4b1](https://github.com/mcereal/mesh-client/commit/f33d4b1b0804039480a5fae36aa8c2d8befbec87))
* **ui:** a window's cursor, hover, sidebar clicks and a message field ([ee44047](https://github.com/mcereal/mesh-client/commit/ee44047c4e3d6e326899490bca58d655716e5836))
* **ui:** carry the whole node roster in the Nodes list ([a5bb507](https://github.com/mcereal/mesh-client/commit/a5bb50793d15d0ccda242fe2e1f768040e035fbf))
* **ui:** centre a split frame's empty detail, and ring one place while typing ([37d78b9](https://github.com/mcereal/mesh-client/commit/37d78b9687684beeec388f8d4215091ca8457f08))
* **ui:** dock the keyboard under the thread on a wide frame ([ac7cd54](https://github.com/mcereal/mesh-client/commit/ac7cd547f46b74e1a19d5f96ddc2221bcfa265d1))
* **ui:** give the map and the waypoints a tab of their own ([948ec81](https://github.com/mcereal/mesh-client/commit/948ec81a0788d347eb10783079cf7a5c560b8986))
* **ui:** head a direct conversation with the peer's long name ([01b9419](https://github.com/mcereal/mesh-client/commit/01b94192e24c5840584a188d321469f710210df1))
* **ui:** lay the tapback sheet out as a row of faces over a delete button ([bfc1ade](https://github.com/mcereal/mesh-client/commit/bfc1adeafd2d35d5e8b551ec429cd8ec5008c5a8))
* **ui:** list, open, compare and delete the radios' backups on the card ([92851bd](https://github.com/mcereal/mesh-client/commit/92851bd088f1856a4e7dc699b4dd7e424dfcace2))
* **ui:** open a node's detail on its signal and battery ([3cdad96](https://github.com/mcereal/mesh-client/commit/3cdad96b694f49364b1e841f00637ab1b269f191))
* **ui:** put the Nodes list's search, filter and sort on one chip bar ([ed3bc2c](https://github.com/mcereal/mesh-client/commit/ed3bc2c575c476a11b7e57de8ac47f1f14f169d5))
* **ui:** say a radio-reported switch as a state, not a control ([8603df6](https://github.com/mcereal/mesh-client/commit/8603df6216240bdf08e325870fa5f10c891db730))
* **ui:** stop heading our own channel bubbles with "You" ([2168e19](https://github.com/mcereal/mesh-client/commit/2168e197652848fd907e8e475060100c30e5de09))
* **ui:** take inkcell's vector emoji ([8dab891](https://github.com/mcereal/mesh-client/commit/8dab8919d4fc8b2255ba62222ff13b479d160dee))
* **ui:** use a wide window's room on the Status and Map tabs ([1076ce3](https://github.com/mcereal/mesh-client/commit/1076ce3011a814ddc536695610f35c6c2e60bc85))

### Bug Fixes

* a PIN typed over a pending Random makes MeshCore pairing Fixed ([ac22b08](https://github.com/mcereal/mesh-client/commit/ac22b08ba8529bb30b75ae514e27563a3b94ef38))
* **actions:** name A on a Radio page only when the row is a verb ([74833cb](https://github.com/mcereal/mesh-client/commit/74833cb10b3ff02555ba71812240a9f98fcdd60c))
* **admin:** stop an edit transaction whose begin fails, and count a refused commit ([d26d82b](https://github.com/mcereal/mesh-client/commit/d26d82b59449e7aa36a91776c3e9687dfea7c047))
* an erased MeshCore PIN takes its Fixed edit with it ([40129d1](https://github.com/mcereal/mesh-client/commit/40129d1b1eb7e41a1be7c5d802f46937bfad6455))
* **app:** count the attached radio again after a stats reset ([e024fc1](https://github.com/mcereal/mesh-client/commit/e024fc15069d85bb1b6e6cd1423aedcd7374b59d))
* **app:** size the BLE handoff addresses by INKWELL_BLE_ADDRESS_MAX ([da3a791](https://github.com/mcereal/mesh-client/commit/da3a79170d39929e04bcc8813526936ffa6540b4))
* **backup:** compare what a MeshCore radio advertises itself as ([cf490e6](https://github.com/mcereal/mesh-client/commit/cf490e6b0e8ef039fce9fe4283d300d2ebe0056d))
* **backup:** keep every contact a MeshCore radio can report ([741640c](https://github.com/mcereal/mesh-client/commit/741640c945f01e7b8ab0b06a04561eaf0f5648dc))
* **backups:** a restart is not a refused transaction ([60574c1](https://github.com/mcereal/mesh-client/commit/60574c1bda991680a47ec00bfad7183f720cb27a))
* **backups:** fit the restore sheet and its firmware note in every language ([a48e16e](https://github.com/mcereal/mesh-client/commit/a48e16e4e0666679e011edf3a095c82211d31aaf))
* **backups:** keep a late key off the queue, never wait for ever, wipe keyed backups on free ([88ba369](https://github.com/mcereal/mesh-client/commit/88ba369f4266134b67e26ae8f60911ef9a90eaad))
* **backups:** list the first-connect backup when the list is full ([7a1a7bd](https://github.com/mcereal/mesh-client/commit/7a1a7bd9d672a0ce2d7a2ccb21137418aad1f71a))
* **backups:** make restores finish, and keep what a restore needs ([1c1ce9a](https://github.com/mcereal/mesh-client/commit/1c1ce9af5e97e045e4e736dc15e8788c39ff8f8d))
* **backups:** name a choice in a comparison as its settings row does ([cf4d3ee](https://github.com/mcereal/mesh-client/commit/cf4d3eece6824d1756f165145247e15e15693e6c))
* **backups:** name every field the Settings tab has in a comparison ([73861e9](https://github.com/mcereal/mesh-client/commit/73861e98c20ff1880f60e700c44962254a3a54ca))
* **backups:** name Radio UI's theme, GPS format and clock face in a comparison ([824a32e](https://github.com/mcereal/mesh-client/commit/824a32e368d666ca3dbdad4658a7d7bcd76002e9))
* **backups:** never call an identity restore good without a public key to judge it by ([d8fec25](https://github.com/mcereal/mesh-client/commit/d8fec2556f517a804b8b63812c9fd3c6790e51c7))
* **backups:** put a key only on the radio its sheet was about, and resync after an export timeout ([0ac0671](https://github.com/mcereal/mesh-client/commit/0ac0671f86126d78899b0f1184699b40691d1985))
* **backups:** refuse a restore without its safety copy, and keep the backup it needs ([fe04be5](https://github.com/mcereal/mesh-client/commit/fe04be550818089cc33166be4c4fa0d97cfbe1e2))
* **backups:** resync after an unanswered key import, and wipe the admin queue as it empties ([22c8a27](https://github.com/mcereal/mesh-client/commit/22c8a27596cbb72a074c1ab8f1ef8831bf3404b8))
* **backups:** stop only a restore's contacts, and count a refused send once ([7e5ffa6](https://github.com/mcereal/mesh-client/commit/7e5ffa6aa143a24ac09d0e62da93e743074303bc))
* **backups:** tell two backups taken in the same minute apart ([cd6a1ef](https://github.com/mcereal/mesh-client/commit/cd6a1efd8299cf23ac9a80bd4841bc9ac2940a10))
* **backups:** wait for a restarted radio even when its link dropped first ([4779d07](https://github.com/mcereal/mesh-client/commit/4779d072db5191c0b6e31bc5861a39bed1eb2384))
* **backups:** wipe the file codec's scratch buffers after a key goes through them ([7e4a28c](https://github.com/mcereal/mesh-client/commit/7e4a28cff16180a56bb7b6061d37fc1db38ce39e))
* clear the gcc warnings a release and a sanitizer build print ([a5844ac](https://github.com/mcereal/mesh-client/commit/a5844aca9ee8e03a2d6e87996af6c987ced7778e))
* clear the remaining gcc and clang build warnings ([9370873](https://github.com/mcereal/mesh-client/commit/9370873ee9488c6c80a226d9756eed546ba083e4))
* **compose:** head the compose sheet and keyboard with the thread's name ([a8df7d0](https://github.com/mcereal/mesh-client/commit/a8df7d05cddb58165bc924fdf1302879a575347f))
* **devtools:** map_pack.py info takes coverage from the deepest zoom too ([061964e](https://github.com/mcereal/mesh-client/commit/061964e61ca9602778c3905f4fb85862f4c136dc))
* do not step the report parser one past the end of its buffer ([cbabe0c](https://github.com/mcereal/mesh-client/commit/cbabe0c79faef5d799ce81c2b8d2a91fba73fa05))
* end a MeshCore link whose handshake loses a step ([5c581fe](https://github.com/mcereal/mesh-client/commit/5c581fe925ecfb442c43e7199006c0c1c0736028))
* find a long-named radio by its truncated advert, and drop impostor bonds ([45088c7](https://github.com/mcereal/mesh-client/commit/45088c731c6cf67b1d3695a7d32c27e81fc2e04b))
* follow a rename into the Bluetooth handoff's name ([a250c5c](https://github.com/mcereal/mesh-client/commit/a250c5c66bd1ea9257054e1edaa2e2af082eb41d))
* hold a typed MeshCore PIN and Pairing as one edit ([f380897](https://github.com/mcereal/mesh-client/commit/f38089738d364e7d097ef94ae809d30e3294013e))
* hold the Bluetooth handoff while its write can still be resumed ([b50979b](https://github.com/mcereal/mesh-client/commit/b50979b90efdb70ae3cb9c496d4ce4a89598c0db))
* hold the next handoff candidate until an impostor's bond is gone ([56216e9](https://github.com/mcereal/mesh-client/commit/56216e977f8e1997fc70e2ff65c74aaf9df1675b))
* keep retrying removal of an impostor's handoff bond ([30fde1e](https://github.com/mcereal/mesh-client/commit/30fde1e7e6923961cc1f7c78957fa505ad18e239))
* keep the Bluetooth handoff until the radio is reached ([43d97f1](https://github.com/mcereal/mesh-client/commit/43d97f1bbcefe17e5c8d00348ea8917ef2ee7fa7))
* keep the MeshCore PIN note inside the help panel's cap ([78bcf34](https://github.com/mcereal/mesh-client/commit/78bcf342c7bd87dd909982b92c0208374f7699ee))
* know the moved radio by its key, and pair it afresh ([5b500f8](https://github.com/mcereal/mesh-client/commit/5b500f8a7a65729bbdae29e528cb427b77cf5372))
* let the Bluetooth handoff outrank a cable and the network ([f5119ce](https://github.com/mcereal/mesh-client/commit/f5119cec17c0f4b3ed6209173bd6b8d7e6140606))
* **lifetime:** measure a hearing after its payload, retry failed appends, count CLI radios ([d895552](https://github.com/mcereal/mesh-client/commit/d89555272f9175314b8bfd5065c90408fcea3639))
* **lifetime:** remember across a restart that the node set is a floor ([ff11fb7](https://github.com/mcereal/mesh-client/commit/ff11fb7eec5a3cc7b15ec422171d86cdebda2007))
* **lifetime:** say whether a record was measured, not just its value ([dc0fb35](https://github.com/mcereal/mesh-client/commit/dc0fb3576bdf2a48d0708f71d5fcfaa850bd1ccc))
* list a silent radio's boards beside a cached radio ([1028e54](https://github.com/mcereal/mesh-client/commit/1028e5493ee4cc4b6747ac59a370446b204842c2))
* **macos:** resolve without forking, and upload a dSYM ([b7b7c61](https://github.com/mcereal/mesh-client/commit/b7b7c61fc8b48c7a54f3182d095fe76a6270f8ae))
* **main:** keep the app off main()'s stack ([dbf4158](https://github.com/mcereal/mesh-client/commit/dbf4158db099edc0ff599d48c5c349fb49b3e27f))
* **map:** open on our own radio, and draw a position's footprint see-through ([dce7983](https://github.com/mcereal/mesh-client/commit/dce7983e82f59cba5ac22bfe40f9faf57a958a51))
* **map:** open the installed map packs again ([982e33b](https://github.com/mcereal/mesh-client/commit/982e33bfc2120430a7145fc8936d5d3d94ec7fa8))
* **map:** order a maps directory ignoring case, over every name in it ([b480f65](https://github.com/mcereal/mesh-client/commit/b480f65206fc2af70a5eeaf62a44c6d81043197e))
* **maps:** count the legacy pack, skip styles the map does not draw, keep an aimed map aimed ([eeae42d](https://github.com/mcereal/mesh-client/commit/eeae42d51073def0b59c7fda01aa8ce21d4b1216))
* **maps:** download only what the map will draw, into the directory it reads ([4beb262](https://github.com/mcereal/mesh-client/commit/4beb262f5044934da106cd5c13f2a13e77d15ceb))
* **maps:** show the catalog a group at a time, count every pack, report a refused delete ([3242c53](https://github.com/mcereal/mesh-client/commit/3242c538bd6575969d77563fdeebf84f4bcbcbbc))
* **map:** take a pack's coverage from its deepest level ([6ddcb43](https://github.com/mcereal/mesh-client/commit/6ddcb43ab6ac3297456bfe9bae82dbd1c3b61b4a))
* **meshcore:** a late reply answers the command it was for, not the next one ([e837f91](https://github.com/mcereal/mesh-client/commit/e837f91645774fa959d2f235864cec3ddf63dd9f))
* **meshcore:** a repeater's reply settles the command sent first, not the lowest id ([4cb7015](https://github.com/mcereal/mesh-client/commit/4cb70155811e045ad9c56e5fec5aed7239891da1))
* **meshcore:** a reply settles only a command the radio has sent ([5a8ea51](https://github.com/mcereal/mesh-client/commit/5a8ea51135156b625186e5a65570537bb296826b))
* **meshcore:** finish the sync when a slow first boot answers twice ([75845ed](https://github.com/mcereal/mesh-client/commit/75845ed0c61a5125caff7d861d5e4d26b1872299))
* **meshcore:** refuse a command rather than evict one still in line ([f1df30d](https://github.com/mcereal/mesh-client/commit/f1df30d8ef2e3136bc75953df2e4ef6eb2917274))
* **meshcore:** refuse a restore that cannot name its radio, and count names it cannot send ([ec233cc](https://github.com/mcereal/mesh-client/commit/ec233cc5a9de665c60e08de0420a7ab799f0c1b5))
* **meshcore:** restart before anything queued behind a key import goes out ([63a6d45](https://github.com/mcereal/mesh-client/commit/63a6d45ab1ab6d319f64496f28b57313ab706642))
* **meshcore:** unanswered commands free their slots, and each timeout is said ([d0872f6](https://github.com/mcereal/mesh-client/commit/d0872f6e1ccd75335eb9ec6e01658a43bd86db9a))
* name the confirm sheet's choose keys from the way its answers were drawn ([60422d0](https://github.com/mcereal/mesh-client/commit/60422d007681e7d0db36d60b232da7a0f0cc83b7))
* **nodes:** a completed sync is a baseline, so a first neighbour is news ([2b19208](https://github.com/mcereal/mesh-client/commit/2b19208f27097e248c9257bf923f6a9cb0d27f7c))
* **nodes:** a discovery seen on the list mid-sync is not announced after it ([51f226c](https://github.com/mcereal/mesh-client/commit/51f226cc3f1aad8f3ad018bf7aac47f66dc24326))
* **nodes:** a MeshCore contact import is not a discovery, and one list predicate ([db509d4](https://github.com/mcereal/mesh-client/commit/db509d4ff1cba51a3674d63471846221ce576911))
* **nodes:** a node opened from the map is not a visit to the list ([230dfd4](https://github.com/mcereal/mesh-client/commit/230dfd4f996469d1fa2a16bdd0b15c5a664bcb31))
* **nodes:** a replayed node is news only when heard since the roster's newest ([719cc25](https://github.com/mcereal/mesh-client/commit/719cc253e6201d5286afe2f849d5f22082048a25))
* **nodes:** an open node covers the list, and does not end the visit ([806a836](https://github.com/mcereal/mesh-client/commit/806a8363b94ef1ec91633cd9b15651072c0a7fe4))
* **nodes:** draw a node's verbs measured over a detail in two columns ([ff3d0dc](https://github.com/mcereal/mesh-client/commit/ff3d0dcac10d42c85bf804c71a301310c8891cfb))
* **nodes:** hold a discovery notice for a completed sync, and queue it ([292f043](https://github.com/mcereal/mesh-client/commit/292f043b24a64acd5d8ccd5918788dcc976425d8))
* **nodes:** keep a first sync first across a retry, and know ourselves from the cache ([06065c2](https://github.com/mcereal/mesh-client/commit/06065c215f7c668660d5fa7f9fe7fda23fba2448))
* offer the Bluetooth move only once the radio has named itself ([a084767](https://github.com/mcereal/mesh-client/commit/a0847674a61bade543a0f701cfe8ba9f14a9b7ee))
* parse a crash report from terminated line copies ([9973a81](https://github.com/mcereal/mesh-client/commit/9973a8178307e239978b83f5802652659fd00d31))
* **profiles:** apply a profile to the radio it was compared with, not the one on the link ([f7a5dc5](https://github.com/mcereal/mesh-client/commit/f7a5dc51aabb4669a2bd70efba3e4b8ebd9b9d33))
* **profiles:** carry an empty channel slot with empty settings, as the firmware does ([3dc9a07](https://github.com/mcereal/mesh-client/commit/3dc9a0749f33f400467952f255571193483c6049))
* **profiles:** claim an export's file name as it is created, and build on Windows ([bb8a645](https://github.com/mcereal/mesh-client/commit/bb8a645ced233e0b5d4210f9f92624265a4f1569))
* **profiles:** end an apply's screen when another radio connects before it is judged ([163c5bb](https://github.com/mcereal/mesh-client/commit/163c5bb271746fa4ff79a46b9c3e11010f272e33))
* **profiles:** export beside an existing .cfg, and keep the newest profiles in a full list ([8d3f1bb](https://github.com/mcereal/mesh-client/commit/8d3f1bbb5b68e57d1e490f5708190bc408e338b5))
* **profiles:** leave the Position section out of a .cfg export ([8d1cae2](https://github.com/mcereal/mesh-client/commit/8d1cae20d42392e75d7dfcf14cf24020ecf68c62))
* **profiles:** read the folder again when Profiles opens, and keep a profile being applied ([ee2474b](https://github.com/mcereal/mesh-client/commit/ee2474b3489fa337221e4d0c2d475d2ed838dc9d))
* **profiles:** refuse an empty .cfg, and judge an apply beside another open comparison ([5964594](https://github.com/mcereal/mesh-client/commit/596459452c54619bfff534e00e2183d3c3d34974))
* reach a silent USB radio on a Brick that remembers a Bluetooth node ([86c53f6](https://github.com/mcereal/mesh-client/commit/86c53f6f823aede402814122dba41d65c9f4a9fe)), closes [#396](https://github.com/mcereal/mesh-client/issues/396)
* retry a failed Bluetooth check, and pass over every impostor ([ac17802](https://github.com/mcereal/mesh-client/commit/ac17802e35e87b865e3bf5b14f06c956883fb793))
* say a failed firmware list failed once in the log ([6cf52cd](https://github.com/mcereal/mesh-client/commit/6cf52cd8e8967fdf2d4b1589ac13ab51ef63b0e1))
* **settings:** say "from preset" only while a preset is on ([def4f22](https://github.com/mcereal/mesh-client/commit/def4f2271a1227d891af7c4e57a10224eae43e1c))
* **settings:** show a preset's LoRa trio as the preset's, not 4/0 ([3a7f662](https://github.com/mcereal/mesh-client/commit/3a7f662808e994ac1a791bb69ec904f7a8512901))
* show a typed MeshCore PIN as Fixed before it is saved ([087f5a6](https://github.com/mcereal/mesh-client/commit/087f5a66e4f2d2a253a446891f9d03f6adcbf1ff))
* silence the two warnings the device build prints ([44c23a2](https://github.com/mcereal/mesh-client/commit/44c23a28e6b701e1a7ab38e0b74a6c7c3c67f955))
* **split:** clamp the preview's nav so a thread previews where it opens ([e849151](https://github.com/mcereal/mesh-client/commit/e8491518e04b5568ff11ec6229d22003438f7cc0))
* **stats:** count direct deliveries by where each message is now ([606447f](https://github.com/mcereal/mesh-client/commit/606447f884bd4988ee6e887b3c060017c695b935))
* **stats:** drop an SNR no radio could report before it becomes a record ([bb5db2a](https://github.com/mcereal/mesh-client/commit/bb5db2ad9779a2baa0bfe53a801392093b8c540c))
* **stats:** keep record holders through an older build ([9ce2210](https://github.com/mcereal/mesh-client/commit/9ce2210609f5a0176368082c7883f1b9e93820b7))
* **stats:** move only the deliveries this run counted ([bc9b0b3](https://github.com/mcereal/mesh-client/commit/bc9b0b3418c054f11ad6e621a8f2ced7d400288f))
* **stats:** reuse a delivery slot only once its message leaves the log ([5c58d28](https://github.com/mcereal/mesh-client/commit/5c58d288c124a6db46aca2b1e4a8d6d25ef1e2e1))
* the confirm sheet chooses with Left and Right, and says so ([7c4e20b](https://github.com/mcereal/mesh-client/commit/7c4e20bab8bd99e440662622719d28fe85168c5c))
* tie a tried bond to the address it was tried at ([24b0515](https://github.com/mcereal/mesh-client/commit/24b051513e3a4128690d2506f107e90e710007b6))
* **ui-drive:** show the cursor after a key the control socket pressed ([e62562b](https://github.com/mcereal/mesh-client/commit/e62562bb62c3ef339989f9d06db3dfd1af35bb82))
* **ui:** a click on a heading beside a detail leaves nothing ([d8e96fd](https://github.com/mcereal/mesh-client/commit/d8e96fd7092703c7dea282159304d02fd96ae7df))
* **ui:** a click on the sidebar's selected row is no change ([089d4a4](https://github.com/mcereal/mesh-client/commit/089d4a42d74ccaf9da4e5a5454c222fad3552e86))
* **ui:** a repeater's failed command is not offered to resend ([13436a6](https://github.com/mcereal/mesh-client/commit/13436a6fe5e0255796b84650ee94e6279a691df4))
* **ui:** address review on the inactive rows, ham power and map counts ([afd6c16](https://github.com/mcereal/mesh-client/commit/afd6c166acb3fe9b8ffbd15d30ca69c3686df39c))
* **ui:** clearer hint labels and radio terminology ([825a6b8](https://github.com/mcereal/mesh-client/commit/825a6b8d25cc71c0d4bae4d83e77df42a0900107))
* **ui:** count only the reader's own narrowing in the Nodes heading ([2f7dc58](https://github.com/mcereal/mesh-client/commit/2f7dc5814efd484547ca7353c0f416a126bfcc7f))
* **ui:** decide the keyboard's dock from the current geometry ([58648ed](https://github.com/mcereal/mesh-client/commit/58648eda933ad051b8997455ec7a03068ce12a50))
* **ui:** draw a traced route's hops behind a real arrow ([5a1b25b](https://github.com/mcereal/mesh-client/commit/5a1b25b5ea9462644063699ae853090bb4f4f9ff))
* **ui:** find the sidebar's open row by identity; no sidebar click under a layer ([703ca39](https://github.com/mcereal/mesh-client/commit/703ca39e4feebc35d2e6eb83ff64d2921028264b))
* **ui:** handle wheel scrolling across list routes ([1aa5467](https://github.com/mcereal/mesh-client/commit/1aa5467f5380338ff219abb16deb27b856bce838))
* **ui:** keep message text inside its bubble ([d62c4e4](https://github.com/mcereal/mesh-client/commit/d62c4e4700f3d31a8ef3a9ec890ae1da99301f35))
* **ui:** keep node detail chips inside their rows, space bubbles apart ([b2b5436](https://github.com/mcereal/mesh-client/commit/b2b54368b9cb016d48c26d3cf957f8d7eeca85ce))
* **ui:** keep the reaction tray and delete inside a narrow sheet ([4276556](https://github.com/mcereal/mesh-client/commit/4276556a37d76c6c3f8aad971f2c601ce9036eed))
* **ui:** line help's paragraphs up under its heading on a window ([7bbbe27](https://github.com/mcereal/mesh-client/commit/7bbbe27115ab8e670bee56edf25c9b0bbe18e843))
* **ui:** name L2 from the cursor, and say quit on overlays too ([81e7af9](https://github.com/mcereal/mesh-client/commit/81e7af92ef203a41351666d58e3f34bac312ca52))
* **ui:** open a map marker's node without the last node's chart or verbs ([3a22ae5](https://github.com/mcereal/mesh-client/commit/3a22ae5cf18dadc30afcd2719e6c4582d6ede7b4))
* **ui:** polish the Radio tab, Settings, the map and help ([00109e9](https://github.com/mcereal/mesh-client/commit/00109e97830f2fd658ae9a3925f4cf45f718c974))
* **ui:** record the new click cases' success; the sidebar's selection is inkcell's ([5ad4b49](https://github.com/mcereal/mesh-client/commit/5ad4b497b520ef77dbe11cfa70819f458df88a19)), closes [mcereal/inkcell#75](https://github.com/mcereal/inkcell/issues/75)
* **ui:** retain wheel key fallback without row targets ([913d92f](https://github.com/mcereal/mesh-client/commit/913d92f9c7347664b01096965aa42de61ca7a210))
* **ui:** ring the bubble a reply answers, not the cursor's ([14e6a20](https://github.com/mcereal/mesh-client/commit/14e6a20549d2b0a354d83db788170153d5158d81))
* **ui:** scroll visible lists with the Windows mouse wheel ([2a16214](https://github.com/mcereal/mesh-client/commit/2a162148b7b250af31854968ee08b4fc0b6ca471))
* **ui:** stop a sidebar click at the discard question; a blank click puts the field down ([35ee895](https://github.com/mcereal/mesh-client/commit/35ee89594d60c4bc03b5fe398dc1424e2f04db40)), closes [mcereal/inkcell#75](https://github.com/mcereal/inkcell/issues/75)
* **ui:** turn the measure back on before the dialogs, not after the action bar ([68529c7](https://github.com/mcereal/mesh-client/commit/68529c7c78263ffb2ebd463bc5ead022ac67dbe6))
* wait for the moved radio, and drop its bond only after it fails ([b342e89](https://github.com/mcereal/mesh-client/commit/b342e8970344b706d001783fd4f11f756e5bc2b9))
* **waypoints:** make the new-place row the bright line on an empty list ([569741e](https://github.com/mcereal/mesh-client/commit/569741ede6d922d138792b775b695516cd148462))

### Documentation

* add status badges to the README ([4c6a684](https://github.com/mcereal/mesh-client/commit/4c6a6849924199c1e87350c7be6990fc137c1d1c))
* describe the Map tab and the Nodes chip bar ([381653b](https://github.com/mcereal/mesh-client/commit/381653b74d71b96ad598f346c6d9aa8e48f8d385))
* say a fixed MeshCore PIN is used by Move to Bluetooth ([660e5a2](https://github.com/mcereal/mesh-client/commit/660e5a24b8a646de50425b02668743e9a5e0c372))

### Code Refactoring

* **ui:** ask the compose sheet's question of a snapshot's own records ([7393832](https://github.com/mcereal/mesh-client/commit/7393832d585c84838eb4727a61fb5a90ce84a23c))

## [2.73.0](https://github.com/mcereal/mesh-client/compare/v2.72.0...v2.73.0) (2026-09-27)

### Features

* a full-screen radio firmware install screen ([8a33501](https://github.com/mcereal/mesh-client/commit/8a335012a42bd64c1ca7a0f3b41b3be3a540e686))
* a status dot and traffic arrows on the footer's link line ([2399e20](https://github.com/mcereal/mesh-client/commit/2399e202eb58b371c03ca429e091a17e8fb44077))
* add a heard MeshCore node as a contact ([60596e3](https://github.com/mcereal/mesh-client/commit/60596e3be19ffc540c5fcb3c5b65f2e48a3cfea3))
* add a MeshCore contact from the signed card the MeshCore app shares ([004705b](https://github.com/mcereal/mesh-client/commit/004705bd536b8b653c831185b7ab54e59de53aec))
* **app:** ask a serial or network link which protocol it speaks ([51c5ca6](https://github.com/mcereal/mesh-client/commit/51c5ca6c6021537495c29497fe325de1ed4261ae))
* **app:** connect to MeshCore companion radios ([40d7d08](https://github.com/mcereal/mesh-client/commit/40d7d087ecdfcd8299e0319fb964868addea6cc4))
* ask a MeshCore contact for its readings ([b2c2207](https://github.com/mcereal/mesh-client/commit/b2c2207848ffe23020c9f6ebf8832edda4d90dfe))
* ask a MeshCore repeater or room server for its status ([7ed7049](https://github.com/mcereal/mesh-client/commit/7ed7049d729b3f93f595ace2ca0d8b1e8710c3c6))
* check and install a MeshCore radio's firmware from MeshCore's own releases ([948827a](https://github.com/mcereal/mesh-client/commit/948827a806318e281a0a480c0e8833eee1c2549b))
* **core:** a session model another protocol can fill ([1cdffbe](https://github.com/mcereal/mesh-client/commit/1cdffbeb9d2071540ee5f5de61dbc7dd58fbd953))
* **core:** MeshCore settings - read, write, refresh and reboot ([934384a](https://github.com/mcereal/mesh-client/commit/934384a1ecd536a591bdb496ba37fe2c64b47206))
* **core:** MeshCore's companion conversation ([15af741](https://github.com/mcereal/mesh-client/commit/15af741435426a846a2d1cb910b9db4736955408))
* edit MeshCore channels - a name, a 16-byte key, and clearing a slot ([fcf45bf](https://github.com/mcereal/mesh-client/commit/fcf45bf2681c15c42c32f831affd8db487ab4ba9)), closes [#name](https://github.com/mcereal/mesh-client/issues/name)
* install a MeshCore nRF52 over USB by the 1200-baud touch ([d9de84c](https://github.com/mcereal/mesh-client/commit/d9de84c313993b35a8bee9576d301f4784740999))
* log in to a MeshCore repeater or room server ([33974b3](https://github.com/mcereal/mesh-client/commit/33974b34a9607f588ddd4a534480e113c316d93c))
* MeshCore contacts - remove one, and advertise this radio ([1ac3542](https://github.com/mcereal/mesh-client/commit/1ac3542132eed4e372b39520c68ce3103b050fb0))
* MeshCore's other parameters - location in adverts, who may request readings, auto-add, multi-acks ([d7954c4](https://github.com/mcereal/mesh-client/commit/d7954c43dfd2b6b1dc13f36ad2523afa61bbd44f))
* **messages:** ring the focused bubble ([b29a8d0](https://github.com/mcereal/mesh-client/commit/b29a8d0cdb9021f376a300c958211f2e22cce834))
* pin a MeshCore contact as the radio's favourite ([481c2a2](https://github.com/mcereal/mesh-client/commit/481c2a277e42cd74a059cdea97147d802cdeb1b3))
* **proto:** MeshCore's companion framing and BLE profile ([5839b7a](https://github.com/mcereal/mesh-client/commit/5839b7a483361ea6e661f8884cbcdb4f3770c78e))
* **settings:** set the client's own section apart from the radio's ([d5d57ab](https://github.com/mcereal/mesh-client/commit/d5d57ab5f0507d4fb09a42d23016d9554f6d4dee))
* share and add MeshCore contacts by the MeshCore app's link ([b9323d6](https://github.com/mcereal/mesh-client/commit/b9323d6db617e7b76763cb0ec6a7dd3c4a6f4712))
* share and join MeshCore channels by the MeshCore app's link ([8c752a1](https://github.com/mcereal/mesh-client/commit/8c752a1868ec39eee7ba98d749f0fc982fdd805a))
* switch a board between Meshtastic and MeshCore over USB ([8056ae3](https://github.com/mcereal/mesh-client/commit/8056ae3794e507c8116cb5ddca88eecd70783126))
* the 1200-baud touch on the Brick, through usbfs ([037aa65](https://github.com/mcereal/mesh-client/commit/037aa65da1e779dc971324694b776c4d8bd0d363))
* trace the route to a MeshCore contact with its path discovery ([759f9dd](https://github.com/mcereal/mesh-client/commit/759f9dd6014269bb731ddb37b28571c24f996d9f))
* **ui:** add a text size setting to About ([cfed5a7](https://github.com/mcereal/mesh-client/commit/cfed5a780476d8f06a27071477272fa9887ca615))
* **ui:** B held goes back to the top of the tab ([8685f07](https://github.com/mcereal/mesh-client/commit/8685f074daff2994f9a91b695d85e32e57cf59fe)), closes [mcereal/inkcell#59](https://github.com/mcereal/inkcell/issues/59)
* **ui:** connect from the empty Messages tab ([1e8c646](https://github.com/mcereal/mesh-client/commit/1e8c646c2c9ccd9da5d978763d72cf7e3db4291f))
* **ui:** dim the LoRa rows the preset setting makes the radio ignore ([4ac2684](https://github.com/mcereal/mesh-client/commit/4ac26849e1202cfc666c0f3a67733049fe30a7c7))
* **ui:** Find on the Nodes list, and a header a line shorter ([6cb8b7f](https://github.com/mcereal/mesh-client/commit/6cb8b7f330d3036f29c5a1285dc001e835882ef8))
* **ui:** gate Meshtastic-only verbs on the protocol's features ([287b707](https://github.com/mcereal/mesh-client/commit/287b7075f2b1b75c6b6594449699f7fe6b3c3eff))
* **ui:** guard one-press sends and disconnects, keep notices, page flat lists ([9384503](https://github.com/mcereal/mesh-client/commit/9384503558ab02b419b2310e02955dc305e25404))
* **ui:** jump to the first unread message and the newest with L2/R2 in a thread ([155a1fe](https://github.com/mcereal/mesh-client/commit/155a1fee8699dbb30a037ce27464dff47ca4f9fb))
* **ui:** keep a draft as a quick reply from the compose sheet ([879e42f](https://github.com/mcereal/mesh-client/commit/879e42fc8e8f4e3747537cc75fb541643acfc604))
* **ui:** MeshCore's settings on the Settings and Radio tabs ([425b657](https://github.com/mcereal/mesh-client/commit/425b65744bafeabc24312c0a47330ec207d53a88))
* **ui:** rule the charts at round values and wash under a node's line ([0b94c75](https://github.com/mcereal/mesh-client/commit/0b94c7513be5944c0973c43f2c84324fa9f8c7a4))
* **ui:** set the compose sheet as a grouped list with a quick-replies heading ([b930704](https://github.com/mcereal/mesh-client/commit/b93070450b17bdbbfe93e1095539029ab46d1c98))
* **ui:** size the desktop window for its display ([380f54d](https://github.com/mcereal/mesh-client/commit/380f54d553fcbde5a08ae12eef8df4d9e25e6f90))
* **ui:** the keyboard's triggers move the caret ([4db32d9](https://github.com/mcereal/mesh-client/commit/4db32d9a4935ac688c9b81478cb220b9eb27f5f6)), closes [mcereal/inkcell#59](https://github.com/mcereal/inkcell/issues/59)
* **ui:** type a node search into the heading, with a live match count ([b8ba1be](https://github.com/mcereal/mesh-client/commit/b8ba1bec18b0d96dbd70fc7750252b2926a95d95))
* update an ESP32 over a USB cable through its ROM bootloader ([3843a48](https://github.com/mcereal/mesh-client/commit/3843a4840237d0e79479a4d1e3642c7efb145177))

### Bug Fixes

* a cancelled ROM write pulses a reset out on the way ([555a17e](https://github.com/mcereal/mesh-client/commit/555a17e5842fad13031dfd6a569250029f63f427))
* a dispatched action is wiped once handled, and a masked caret at the start stays there ([6c7a359](https://github.com/mcereal/mesh-client/commit/6c7a359ba76bfd0912b083d7d53ac783a291604d))
* a failed ROM write resets the chip back into its app before closing ([28a400c](https://github.com/mcereal/mesh-client/commit/28a400c154c88db79d631ca09dd41bba41cc7d6e))
* a masked MeshCore password's counter counts the bytes it really is ([1aea446](https://github.com/mcereal/mesh-client/commit/1aea4463b15039e5d459e22adeeb1dc35ea48e00))
* a MeshCore add's OK does not write over another key heard meanwhile under the same number ([22fef00](https://github.com/mcereal/mesh-client/commit/22fef00b14eb718e2bb9c6579fae43e17eb5b295))
* a MeshCore answer is its build's; the handover holds a radio to its name; any known ESP chip ([3ef3851](https://github.com/mcereal/mesh-client/commit/3ef385180b8338781912ef725e95d32be1f17ad7))
* a MeshCore answer that came straight back is drawn as a way back ([ad7b8fe](https://github.com/mcereal/mesh-client/commit/ad7b8fecfdeef16af5f8ae91491f350c1b150e42))
* a MeshCore channel is editable only once this handshake read it, and a chosen zero key is a key ([37de8be](https://github.com/mcereal/mesh-client/commit/37de8bee2e35abd6ab13c82e4263ca687455041b))
* a MeshCore contact brought back by the radio's OK is heard as of the OK ([5399e53](https://github.com/mcereal/mesh-client/commit/5399e533cd30b4bba2a40614006b3a587a8947e1))
* a MeshCore contact brought back without network time is stamped by the radio's clock ([b2eb408](https://github.com/mcereal/mesh-client/commit/b2eb4089ce324e1333f277ef42a31e351ce4b39a))
* a MeshCore contact leaves the list when the radio agrees, and only a contact offers Remove ([df88648](https://github.com/mcereal/mesh-client/commit/df886480a7a1b473e00b7eafa7070c8bb35c4868))
* a MeshCore contact link is checked against adds still waiting, not only the roster ([d16412d](https://github.com/mcereal/mesh-client/commit/d16412d6781d93f8e683a3ef406fa99a9e8a2c50))
* a MeshCore contact link whose key starts as another node's is refused, not stored over it ([ddeb245](https://github.com/mcereal/mesh-client/commit/ddeb2459c0285148f551f551aa365b7d129dba75))
* a MeshCore contact the radio accepted is stored from the record sent, even if the roster let it go meanwhile ([7d4324c](https://github.com/mcereal/mesh-client/commit/7d4324c1920b8b48e44aa34fedf07de71db53192))
* a MeshCore contact the radio dropped is no longer pinned, and Pin is not named over this radio itself ([bca9e8e](https://github.com/mcereal/mesh-client/commit/bca9e8e9020391457be59bdf89f6084f25908e14))
* a MeshCore favourite's write runs where its lookup stood, and the list names Pin only over a pinnable row ([ace11d1](https://github.com/mcereal/mesh-client/commit/ace11d14476f544a89510d276eb919d70dc01e24))
* a MeshCore node heard again is the newest kept advert, not the next evicted ([3281ec4](https://github.com/mcereal/mesh-client/commit/3281ec453f396a5a6e4ce81205f9be8bb2f0a4b6))
* a MeshCore password is masked as it is typed and wiped when the keyboard closes ([4ab49b3](https://github.com/mcereal/mesh-client/commit/4ab49b3203564c87a40f339249b5e559d709be97))
* a MeshCore password is published masked, so no snapshot holds it ([3c95748](https://github.com/mcereal/mesh-client/commit/3c95748698ba5ba8234ab4b852da836dc5f5bc84))
* a MeshCore request answered by anything but a readable SENT ends as not sent ([9586415](https://github.com/mcereal/mesh-client/commit/9586415681a6c2bb42cd432240c2def3aace653f))
* a MeshCore request ends when its write fails or the link drops, and a login's password leaves the queue ([c4d6dae](https://github.com/mcereal/mesh-client/commit/c4d6daed58938c0c6f9d6ce7ab0589fbbf39c347))
* a MeshCore request queued behind a reboot stays open through the handshake's restart ([2e18561](https://github.com/mcereal/mesh-client/commit/2e18561295a84b3758fc807327432dc29877a038))
* a MeshCore telemetry answer ahead of its SENT does not leave the radio locked ([7ab983a](https://github.com/mcereal/mesh-client/commit/7ab983a4a4aeeaf0491c8970323ba5c8d17d8957))
* a MeshCore telemetry answer marks its node heard ([670e776](https://github.com/mcereal/mesh-client/commit/670e77682dbf94e9fa240982eb91b8ce4f3b0df3))
* a named port that is gone falls back only to finish a write; a retry keeps earlier damage ([0d37dee](https://github.com/mcereal/mesh-client/commit/0d37dee263831c8e29e7b60f9ab0149d17d41483))
* a prompt the radio raises over a MeshCore login closes it rather than parking the password ([c62f98f](https://github.com/mcereal/mesh-client/commit/c62f98f695dbd98e1c7a0cf535fe75bd91e5903f))
* a serial update cancelled after its erase stays a radio to recover ([da85de2](https://github.com/mcereal/mesh-client/commit/da85de20ed4be5573696c3619385b0436033de97))
* a switch's recovery press is the switch's, and it is drawn ([7078776](https://github.com/mcereal/mesh-client/commit/70787763e017631a7ce86fb3a179294274742048))
* a whole-flash image's bootloader slot must hold a bootloader, not an app ([86a61c4](https://github.com/mcereal/mesh-client/commit/86a61c435d379d41e423b4d7b18472fffcf71cea))
* Add from a link waits for the MeshCore handshake, and a heard node added by link stays heard ([9f2d637](https://github.com/mcereal/mesh-client/commit/9f2d63700f78b6b4986c7c57ed58f3b820424c12))
* an added MeshCore contact carries the sender's advert stamp and its whole 32-byte name ([23d0527](https://github.com/mcereal/mesh-client/commit/23d05273a23ab3242992aca9a2143eeb778d3c33))
* an early MeshCore telemetry answer counts only for a request already written ([3017dfd](https://github.com/mcereal/mesh-client/commit/3017dfdef0027410f2e75d563b51b8ecd26e0fa9))
* any MeshCore add checks the adds still waiting under its number, whether or not the key has a row ([021b780](https://github.com/mcereal/mesh-client/commit/021b780a964e1328204f882023d5cb4d9113cf14))
* **app:** clear a MeshCore advert location, and refuse a frequency finer than a kHz ([d9cdfe0](https://github.com/mcereal/mesh-client/commit/d9cdfe0895420ab48b09214771cd0bbbb51e77b4))
* **app:** MeshCore max power is the radio's maximum, and a cleared location clears the fix ([72012bd](https://github.com/mcereal/mesh-client/commit/72012bd20a2a6c46a436984f092703bb68d9efe0))
* **app:** pass over a muted host before the network retry is due ([1839b37](https://github.com/mcereal/mesh-client/commit/1839b379b28eb16a3f45797eb80dc33bfb674ac8))
* bound a whole image by its app partition; a resumed switch keeps its destination ([98fa6df](https://github.com/mcereal/mesh-client/commit/98fa6df0a0287a7531745a1914d61c93f8260555))
* **cli:** speak the app's conversation in --status and --send-text ([b2b3ea5](https://github.com/mcereal/mesh-client/commit/b2b3ea5a8c33e175675cdfe7f90ad18b12b1a9df))
* **core:** a MeshCore command the link refuses on the spot is the caller's answer ([1881654](https://github.com/mcereal/mesh-client/commit/1881654be4bbd2dcdb1bbaddae74ca00a606b5ae))
* **core:** a MeshCore save refused on the spot is the call's answer; OK moves the baseline ([38ad60e](https://github.com/mcereal/mesh-client/commit/38ad60e1da6ed5f06cc3ffc32e3f84e67a1143d3))
* **core:** a post-reboot MeshCore handshake the link refuses drops the link ([06ae083](https://github.com/mcereal/mesh-client/commit/06ae083ba88eea1980d7c1297ccdb6409e1f8737))
* **core:** MeshCore stamps, room authors and an unreadable SENT ([a2d60b3](https://github.com/mcereal/mesh-client/commit/a2d60b37f7b555e8a55fddb3fef996ddf8ccd2ff))
* **core:** no MeshCore save queued behind a reboot ([3baafd1](https://github.com/mcereal/mesh-client/commit/3baafd10e69fa55d5875d77f0f67f5a30a00c718))
* **core:** no MeshCore save while a SELF_INFO read is queued ([6d6a010](https://github.com/mcereal/mesh-client/commit/6d6a0107bdc4ceebfb1917248cdc9c8a23fff5bc))
* **core:** one MeshCore reboot at a time, and a refused one says so ([6b80bb0](https://github.com/mcereal/mesh-client/commit/6b80bb04a8c53e77adaf7f963f5ceea929b487f2))
* **core:** settle a MeshCore settings command the link refuses ([ca508ed](https://github.com/mcereal/mesh-client/commit/ca508ed6bce045a636347619c978e8428faa519f))
* **core:** show an accepted MeshCore write without its read-back; refuse a Set that rounds to 0,0 ([c324c58](https://github.com/mcereal/mesh-client/commit/c324c5866b00262ceff33df44f75ae8c1a2c5d0a))
* drop a firmware answer the moment the link speaks the other protocol ([b859e8a](https://github.com/mcereal/mesh-client/commit/b859e8a369d47e9765c48eeedb6988e628303beb))
* each MeshCore favourite lookup carries its own answer, and its write takes the slot the lookup leaves ([44494cd](https://github.com/mcereal/mesh-client/commit/44494cd859f02bc0533aef0bf8aa3285436de258))
* every copy of a MeshCore login's password is wiped, with a store the compiler keeps ([fe9d780](https://github.com/mcereal/mesh-client/commit/fe9d78069905c41aa901588900fd57f7378a0c5d)), closes [mcereal/inkwell#49](https://github.com/mcereal/inkwell/issues/49)
* every row of a repeater's status fits its row, in every language ([ae9f8cb](https://github.com/mcereal/mesh-client/commit/ae9f8cbbd10c2f1972ff63ae7815569193182d09))
* give link traffic its own update flag ([6c550aa](https://github.com/mcereal/mesh-client/commit/6c550aab0ab576ad5fa1b4502ad24b8bbb9a9737))
* give the crash report's real path on the Brick ([8b70d64](https://github.com/mcereal/mesh-client/commit/8b70d647acd416fc09d6ff49f4ae45ee2ec3b270))
* **help:** describe the Nodes list's default order as it is ([579bf23](https://github.com/mcereal/mesh-client/commit/579bf2339d958f92bbdcaf29331e5238e9234fa9))
* latch an erase where it is sent; a resume falls back to the only bridge ([e38e221](https://github.com/mcereal/mesh-client/commit/e38e221f79939291c99dfab4b4087cb8ac68e09c))
* MeshCore adverts kept for adding are cleared when a connection begins ([a9da322](https://github.com/mcereal/mesh-client/commit/a9da32261b102e27901dbe39dcdee5dfef81cd3b))
* MeshCore current and voltage are signed, each answer replaces the environment, a queued request holds the lock ([b080c1a](https://github.com/mcereal/mesh-client/commit/b080c1a0510ef0b01cb6ea13ef07ab3db4cee554))
* MeshCore current in mA, GPS off the globe refused, one telemetry request at a time ([0bffbbd](https://github.com/mcereal/mesh-client/commit/0bffbbd7e46f9a5f96536b28b4ed5128b896eebf))
* **nodes:** make node detail read less like a protocol dump ([b25a95d](https://github.com/mcereal/mesh-client/commit/b25a95d89c7a089fa94081a772c60f0c3e1da634))
* **nodes:** round coordinates for every stated precision, not only the table's ([3564cb6](https://github.com/mcereal/mesh-client/commit/3564cb6ba0555e019bf676f45c687310cf4348e0))
* **nodes:** shorten the empty Waypoints row ([3266565](https://github.com/mcereal/mesh-client/commit/326656590ef5f5ac2048b6bb6bdaf343a7652fd3))
* offer a firmware switch only where it erases, which is an ESP32 ([411a3e7](https://github.com/mcereal/mesh-client/commit/411a3e709ead19692d6b9ad2ee42a7b9f86b8c4e))
* only a MeshCore contact the roster let go is re-stamped as heard on the OK ([a1c008e](https://github.com/mcereal/mesh-client/commit/a1c008ef76e146dfa12169f6b3e4e4ca4807986b))
* only the asked MeshCore node's answer frees the telemetry lock, and a 0,0 fix is a position ([635a7db](https://github.com/mcereal/mesh-client/commit/635a7db82866338ce7268ce26ada83d907a5df67))
* put the submodule pointers back where main has them ([9a77350](https://github.com/mcereal/mesh-client/commit/9a77350a9b666e4c5f889a2e3db81ff11aae0185))
* read a flasher device's builds once its name and type are known, in any key order ([3ca53de](https://github.com/mcereal/mesh-client/commit/3ca53de446e4031a83429c98c6f31da0f3b02543))
* refuse a whole-flash table declaring past the largest ESP32 flash ([704f508](https://github.com/mcereal/mesh-client/commit/704f5083230e534a1460e16505661f4adc0382c1))
* refuse the 1200-baud touch on a generic-driver port it cannot reach ([3c99b36](https://github.com/mcereal/mesh-client/commit/3c99b360c3ea137d8e14b954326d8b6d99cef31c))
* refuse USB to an ESP32 on its own USB before the install; reset out after a port failure too ([a89cdb3](https://github.com/mcereal/mesh-client/commit/a89cdb3c0a0111f88000c7dcb4ed8a8cad454eab))
* remove a MeshCore contact only once the radio is named, and word the advert as asked ([71f506c](https://github.com/mcereal/mesh-client/commit/71f506cd4c9dad9ddd8467abb486d8ad1d421c6c))
* resume a direct-image install; refuse a partition overlapping the image ([f400f1a](https://github.com/mcereal/mesh-client/commit/f400f1a959e4461ced1d5dab63313f6886fa60c2))
* the install screen keeps its release and title through the result ([096ae50](https://github.com/mcereal/mesh-client/commit/096ae50483bee1e46daeb5b7ee6cb6b2f840f031))
* the keyboard holds the longest MeshCore contact card whole ([1974503](https://github.com/mcereal/mesh-client/commit/19745037c5fc7abb7082716c7c36952996da732d))
* the MeshCore channel import walks its slots with a counter as wide as its bound ([398486d](https://github.com/mcereal/mesh-client/commit/398486d89030ef2c1163dfcd8be1b43f2ed7d6b3))
* the Spanish add-contact row fits its label ([cfbd9e9](https://github.com/mcereal/mesh-client/commit/cfbd9e9738c3935783224db72465f3696e09aaa1))
* the write's progress is not full until the trailing erases verify ([9187dc7](https://github.com/mcereal/mesh-client/commit/9187dc758fdfcd589f7ee386f7f75e0984d5664f))
* **transport:** bound notified frames by the profile, list every profile in full ([b16f753](https://github.com/mcereal/mesh-client/commit/b16f75316f45b62616b5d997acbd707ab9260695))
* **transport:** list a Nordic UART device only when it is named like MeshCore ([c8e2fc6](https://github.com/mcereal/mesh-client/commit/c8e2fc6c44e1f3a52862e829f8558b906e9e0715))
* **ui:** a list heading counts only what the list is not showing ([70b12a6](https://github.com/mcereal/mesh-client/commit/70b12a64f4e87e9292216f4af3db3c9745fdd790))
* **ui:** a security prompt over the Find keyboard is drawn, not the search ([9a228cb](https://github.com/mcereal/mesh-client/commit/9a228cb745917d74bab34dd48cdd989020f8b1ea))
* **ui:** arm a disconnect against its radio; page with a map left on Nodes ([a6cf715](https://github.com/mcereal/mesh-client/commit/a6cf71578e47f91f616f0555758a920e0bf7fed6))
* **ui:** colour a device's disc only when the colour means something ([268831c](https://github.com/mcereal/mesh-client/commit/268831c5e432d0177a6e36b74cf04e231f695e30))
* **ui:** count conversations, not rows, and say when one is empty ([8e13fe2](https://github.com/mcereal/mesh-client/commit/8e13fe2a8a9c46642e22bcd27214954fd27cf1f5))
* **ui:** gate the Nodes pin shortcut on the protocol's node flags ([e50ceba](https://github.com/mcereal/mesh-client/commit/e50cebaf950046cce4ff96c45dfee9d5aeaee4d7))
* **ui:** hold a MeshCore draft to what the radio will send whole ([1abcfc6](https://github.com/mcereal/mesh-client/commit/1abcfc63263faf74eb6d4e01e0ac4dad4d6bb96e))
* **ui:** keep an unsent draft with the conversation it was written for ([6d193f6](https://github.com/mcereal/mesh-client/commit/6d193f68ff0f7e36e7778eff0577f0c1f2bba5e6))
* **ui:** label our own bubbles "You" rather than "sent" ([4df2034](https://github.com/mcereal/mesh-client/commit/4df203442aec1e2c72179c3b99d721c255ab63cf))
* **ui:** list MeshCore's channel slots as facts, not rows that open the editor ([30c16d6](https://github.com/mcereal/mesh-client/commit/30c16d6334ec3100f781e46577f48d5be6b35432))
* **ui:** MeshCore power in literal signed dBm, and its frequency to the exact kHz ([0dc865a](https://github.com/mcereal/mesh-client/commit/0dc865a81734bcbd264e66b0663711a865edd43f))
* **ui:** MeshCore's full bandwidth and spread ranges; round coordinates; a lost link is no reply ([638f3d5](https://github.com/mcereal/mesh-client/commit/638f3d566ad88b56ce1b88c12b55e390a7d65997))
* **ui:** name no keycap an empty thread or a one-row picker cannot use ([7063d5f](https://github.com/mcereal/mesh-client/commit/7063d5f4d9a2aa204fc51be8e926b65a8c841ebd))
* **ui:** name only the presses that work, and give the empty Nodes tab a way out ([4b911f0](https://github.com/mcereal/mesh-client/commit/4b911f097c1a66ce095290737e0c2fabd2f430fa))
* **ui:** name the answer A gives in a confirm dialog ([d91b0b6](https://github.com/mcereal/mesh-client/commit/d91b0b6228e00128e68608ec975b4ff7590ebcf4))
* **ui:** offer X delete only on a conversation row ([929fc7b](https://github.com/mcereal/mesh-client/commit/929fc7bda10abeae3811c7075118210228448d42))
* **ui:** refuse the Waypoints row out loud on a protocol without waypoints ([8fcd147](https://github.com/mcereal/mesh-client/commit/8fcd147cb3a73e0c8bd8a5027af440dd494830b6))
* **ui:** say it in the reader's terms on the node detail and three other screens ([77233cf](https://github.com/mcereal/mesh-client/commit/77233cf4b654d3adeacd0d30217659d718e7021f)), closes [#Trail](https://github.com/mcereal/mesh-client/issues/Trail)
* **ui:** say when a dimmed LoRa row's edit will count ([56529f6](https://github.com/mcereal/mesh-client/commit/56529f6fa7f7aace393129eee9f44f164113bde7))
* **ui:** step the text size from a window's display-sized scale ([d316df5](https://github.com/mcereal/mesh-client/commit/d316df5a94619c6c921a5d62ce5f8837f5ebe9c9))
* **ui:** stop MeshCore's power row at the radio's own maximum ([fcddd06](https://github.com/mcereal/mesh-client/commit/fcddd066337f36b520b6070d345bfef8a1ee7d4a))
* **ui:** the Waypoints heading says how many places the book evicted ([4888a89](https://github.com/mcereal/mesh-client/commit/4888a896879369a07122a2adc3128231386ee71d))
* whether a re-added MeshCore contact was heard comes from where the add came from, not its stamp ([a029e80](https://github.com/mcereal/mesh-client/commit/a029e80331e3353112d70916e135162732810638))

### Documentation

* **cli:** the window's scale and size are the display's ([6a86bb9](https://github.com/mcereal/mesh-client/commit/6a86bb924681abc3d3cbf18eea24f91f3d5bfcb1))
* mark the macOS and Windows builds as experimental ([2cfd7a7](https://github.com/mcereal/mesh-client/commit/2cfd7a7bcbb982611602332b7efe5f14126255d2))
* MeshCore as it is spoken, and MESHCLIENT_PROTOCOL ([a54409c](https://github.com/mcereal/mesh-client/commit/a54409c88a79bf1628dd8e37e27e4548870be0a1))
* refresh the listing screenshots ([603ba50](https://github.com/mcereal/mesh-client/commit/603ba50ee71a7bbe2a04a4ae3b827865f22141f2))

### Code Refactoring

* size a contact link's id for either app's spelling, asserted ([1e5e9a3](https://github.com/mcereal/mesh-client/commit/1e5e9a3441749d56a1e66046df640ebf4e8218ce))
* **transport:** BLE link speaks the protocol's GATT profile ([1e0b31f](https://github.com/mcereal/mesh-client/commit/1e0b31fb54236c9bf30639a0d4efceff3d749836))
* **transport:** links talk to a protocol, not to mesh_session ([e4fe7f6](https://github.com/mcereal/mesh-client/commit/e4fe7f64da0c61c1a3058a5c68dfd7d55f3b52ce))

## [2.72.0](https://github.com/mcereal/mesh-client/compare/v2.71.1...v2.72.0) (2026-09-25)

### Features

* **ble:** Bluetooth on macOS, over inkwell's CoreBluetooth backend ([e86a153](https://github.com/mcereal/mesh-client/commit/e86a153c5a2e96805b5604079e26ad0a04a6a7ed)), closes [mcereal/inkwell#12](https://github.com/mcereal/inkwell/issues/12)
* **ble:** wait on FromNum's subscribe across turns instead of in the connect ([6bcf398](https://github.com/mcereal/mesh-client/commit/6bcf398b63ffc8a35f6ae33374bcac6794103dd0))
* build, test and run the UI natively on macOS ([d12166f](https://github.com/mcereal/mesh-client/commit/d12166f378e68e7c336165ce94e4dc5dc9d1cb87)), closes [inkwell#10](https://github.com/mcereal/inkwell/issues/10) [inkcell#28](https://github.com/mcereal/inkcell/issues/28)
* **devices:** honest signal readings, a visible auto-connect radio, safer radio switches ([4cbcb9a](https://github.com/mcereal/mesh-client/commit/4cbcb9a210165c8bf815883bb16248ea4405abef)), closes [mcereal/inkwell#39](https://github.com/mcereal/inkwell/issues/39)
* **firmware:** install nRF52 radios over BLE through their DFU bootloader ([6cc8b53](https://github.com/mcereal/mesh-client/commit/6cc8b53da7dbf910e966f16ea017b9d4783b9559))
* **focus:** a dialog's answers are reached by where they are, not by counting ([9f83bbd](https://github.com/mcereal/mesh-client/commit/9f83bbd8a20d32c1776e7268289738aefe6f7a33)), closes [mcereal/inkcell#19](https://github.com/mcereal/inkcell/issues/19)
* **help:** the notes are a body in pixels, under a heading that collapses ([1ccfebf](https://github.com/mcereal/mesh-client/commit/1ccfebf5da602982e9b2a5cf368049864fdd7608))
* **lists:** the body travels between windows instead of flicking ([9b2484c](https://github.com/mcereal/mesh-client/commit/9b2484cd893eb7b078ba60bd0d11cdf24a0c5b56))
* **overlays:** the two questions arrive on layers, over what they are about ([af2a001](https://github.com/mcereal/mesh-client/commit/af2a00131ab639101cfb847aef9e9f6aa01de2d3))
* **release:** app icon for the macOS and Windows installs ([abcfbd0](https://github.com/mcereal/mesh-client/commit/abcfbd03dde9acf01ba6e95927af33515a8b9ced))
* **release:** ship a macOS .dmg and a Windows installer ([2962f42](https://github.com/mcereal/mesh-client/commit/2962f4243b1aad4ec2f8ea2fe2d7a22725af42ca))
* **serial:** find USB serial ports on macOS ([7def11d](https://github.com/mcereal/mesh-client/commit/7def11d6a9389929440aed1e659c7459b3321e7c))
* **settings:** a row's gutter says how it is changed, not just that it can be ([726687b](https://github.com/mcereal/mesh-client/commit/726687b22bec1a74b7d056a8a36b43574ea3fdd8)), closes [inkcell#3](https://github.com/mcereal/inkcell/issues/3)
* **sheets:** a node's verbs and a message's faces come up over what they are about ([b7bca59](https://github.com/mcereal/mesh-client/commit/b7bca595a348734032cc1c5a47b6e541ccdef510))
* **ui:** a one-row footer - the screen's verbs and the link, nothing the chrome already says ([81b932e](https://github.com/mcereal/mesh-client/commit/81b932ed3956abcb65b88d1497a1b35d67cffce8))
* **ui:** a right-click on a row opens its menu ([20d686b](https://github.com/mcereal/mesh-client/commit/20d686bd0f4b69bc0194c304b83e4060df2fa59f))
* **ui:** a window's action bar is a toolbar, and back is the arrow ([3a73660](https://github.com/mcereal/mesh-client/commit/3a73660e73af5e5861f89f2385fb902245e0ee43)), closes [mcereal/inkcell#36](https://github.com/mcereal/inkcell/issues/36)
* **ui:** bind desktop shortcuts to commands ([562e2b3](https://github.com/mcereal/mesh-client/commit/562e2b35202dbb6f41b8d6ce1c565ecf321dfb91))
* **ui:** draw the focus ring and lift the focused row instead of filling it ([a68d642](https://github.com/mcereal/mesh-client/commit/a68d64283d9fbabe79530685dd444b0ab8fd74bb))
* **ui:** drive the running client by its keys and look at what it drew ([24818d0](https://github.com/mcereal/mesh-client/commit/24818d0b8d6bd5ba4e745565990d007c740e3806)), closes [inkcell#30](https://github.com/mcereal/inkcell/issues/30)
* **ui:** follow inkcell onto a proportional face ([fe34156](https://github.com/mcereal/mesh-client/commit/fe3415639981a5a5e4f5a1f1cb2531544eb3c833))
* **ui:** leave the on-screen keyboard grid out of a desktop window ([42276a8](https://github.com/mcereal/mesh-client/commit/42276a83343e046f1160f3f85080404be755cd37))
* **ui:** merge Devices and Status into one Radio tab ([c23f673](https://github.com/mcereal/mesh-client/commit/c23f6732822595d565850c4b0d06d8edae6409d4))
* **ui:** MESHCLIENT_UI_BACKEND=sdl draws the UI in a window ([ec9be92](https://github.com/mcereal/mesh-client/commit/ec9be92326400643ddb2c1044cecee4d9db06038))
* **ui:** Messages stands a thread beside its conversations on a wide window ([8ce276b](https://github.com/mcereal/mesh-client/commit/8ce276b76cbcc13af85e0fd0377537beafdd4eb8))
* **ui:** move About radio and Radio actions onto the Radio tab ([2551438](https://github.com/mcereal/mesh-client/commit/255143836a4ad875c09d360bddc6dd6db4501c1f))
* **ui:** move Waypoints into the Nodes tab as a row ([e37eeb4](https://github.com/mcereal/mesh-client/commit/e37eeb4064c8b4c61be84a80b7f30ed16c2030f8))
* **ui:** name a node row once and put what else it knows on the line under it ([9e2941d](https://github.com/mcereal/mesh-client/commit/9e2941d3c1805f744010bb36a6964391f05162a4))
* **ui:** Nodes stands a node beside its roster on a wide window ([399061a](https://github.com/mcereal/mesh-client/commit/399061a4c947a0958e3cdfbb13474852f434757b))
* **ui:** place the frame with inkcell's scaffold ([92a32c8](https://github.com/mcereal/mesh-client/commit/92a32c83071599332b9af8fe24acb0d4d5cf281b))
* **ui:** put the Mac window's buttons in the tab strip ([8762102](https://github.com/mcereal/mesh-client/commit/876210222b1cb7b6a075dbdf13795d66aa396e0c)), closes [inkcell#29](https://github.com/mcereal/inkcell/issues/29) [inkcell#29](https://github.com/mcereal/inkcell/issues/29)
* **ui:** route SDL hints through commands ([a08a003](https://github.com/mcereal/mesh-client/commit/a08a0033d3325e7a97de421e8c552424d3b0ddaa))
* **ui:** set every list by what it is for - inset sections, an accent cursor, tiered type ([5b9915e](https://github.com/mcereal/mesh-client/commit/5b9915efab9800d55feca8dd15acea38b0a84367))
* **ui:** Settings stands a section beside the section list on a wide window ([6acdb56](https://github.com/mcereal/mesh-client/commit/6acdb5619d67f6429d5d2faefb65472b470ea99d))
* **ui:** size label columns and the code caption from measured text ([42f2a4e](https://github.com/mcereal/mesh-client/commit/42f2a4ed346570f35f7cb7ef0988d44e56504b06)), closes [mcereal/inkcell#53](https://github.com/mcereal/inkcell/issues/53)
* **ui:** tabs, rows and dialog answers are clicked ([8daf923](https://github.com/mcereal/mesh-client/commit/8daf923ac129a863a95ef364cc30ab47e3d57a8b))
* **ui:** the sheet and the context menu stand off the page; the node's verbs are a menu ([96e3cd3](https://github.com/mcereal/mesh-client/commit/96e3cd3c099273e5f8a6963ea6d63aeb2e97e6e8))
* **ui:** type directly into SDL keyboard drafts ([fec131b](https://github.com/mcereal/mesh-client/commit/fec131b8898ab39ca96bc658b7f0e0633a094cc2))
* **ui:** verbs in the heading for a pointer, a one-row foot on the device ([ba6e616](https://github.com/mcereal/mesh-client/commit/ba6e61664b2dd771fbad73cd82ecc0bac1beb467))
* **update:** install updates on macOS and Windows ([b282f12](https://github.com/mcereal/mesh-client/commit/b282f12f44e4da1d64e92e82ea9a5bc9ba851ac0))
* **update:** say which network failure a check or download hit ([81c6284](https://github.com/mcereal/mesh-client/commit/81c6284b8eba17f6e25fcd2bcfdf5e5b09ca7668))
* **windows:** connect to a radio over Bluetooth ([275990f](https://github.com/mcereal/mesh-client/commit/275990fefc9c80d34cf6760f67406139ee77be22))
* **windows:** connect to a radio over USB serial ([e3153ca](https://github.com/mcereal/mesh-client/commit/e3153ca1cc3b42cae35a99682f6973b5980a4dbf))
* **windows:** hand TCP sockets to native stream ([2de9114](https://github.com/mcereal/mesh-client/commit/2de91146746fb38f78eddd50422dc25a8d447b77))
* **windows:** open SDL UI by default ([a28098b](https://github.com/mcereal/mesh-client/commit/a28098b8a443ec1074051915ec4c5e18bdaad214))
* **windows:** support TCP hostnames ([a03c44e](https://github.com/mcereal/mesh-client/commit/a03c44e13f1542117bf23a98ea408db0f67938b8))
* **windows:** TLS and HTTPS, through Mbed TLS ([0ef3fbd](https://github.com/mcereal/mesh-client/commit/0ef3fbd25f5742b57e433e6308e69de1a410edbe))

### Bug Fixes

* **ble:** address review on scan memory and the preferred-radio streak ([4cc4205](https://github.com/mcereal/mesh-client/commit/4cc4205e62fba223bb683a699e44aea16d4034d5))
* **ble:** demote on revoked Bluetooth access, size OTA addresses for macOS ([412325f](https://github.com/mcereal/mesh-client/commit/412325f78427834ce8946eb3a00432e0a57b9c07))
* **ble:** don't reset the controller over the link a pairing made ([2911db1](https://github.com/mcereal/mesh-client/commit/2911db1409639294ec2d3331fd7b78a24a5c67cb))
* **build:** a clean build, under both compilers ([887f64e](https://github.com/mcereal/mesh-client/commit/887f64ea430d73ea099644c07f661566ed07e00c))
* **build:** compile inkwell the way this client's own sources are ([bb56ccc](https://github.com/mcereal/mesh-client/commit/bb56ccc74e5a078a046cda4d60f54568a2b43683))
* **build:** the no-TLS build must still link ([f596271](https://github.com/mcereal/mesh-client/commit/f5962711d74ebba6b03f78f0df1257049d8db905)), closes [#267](https://github.com/mcereal/mesh-client/issues/267)
* clear the no-TLS and MinGW build warnings ([20e7cf4](https://github.com/mcereal/mesh-client/commit/20e7cf47b893cd935c36a5d744471fea6dbb3e43)), closes [#ifdef](https://github.com/mcereal/mesh-client/issues/ifdef)
* **devtools:** every scene verb plays out what it set moving ([9fba6d0](https://github.com/mcereal/mesh-client/commit/9fba6d030808009f1b64c871112e9a3ea26da61d))
* **devtools:** make `syncing off` end the replay it names ([90aaa1f](https://github.com/mcereal/mesh-client/commit/90aaa1fbcb96af87035b9784b34babd4fa66658c))
* **devtools:** refuse a verb row the runner answers first, and keep --delay 0 ([4fcddc3](https://github.com/mcereal/mesh-client/commit/4fcddc31a99180b5a2b9746fe0af6b478eb590ca))
* **firmware:** don't claim a silent server for every network failure ([168aafc](https://github.com/mcereal/mesh-client/commit/168aafc05b1bdf2883db312a57ec2d3a216df4bf))
* **firmware:** give a silent CDN 15 s, not the image's two minutes ([187dcc0](https://github.com/mcereal/mesh-client/commit/187dcc005ac4ac3fb057601f94e90945d77d7357))
* **firmware:** initialize USB storage writer ([d208be9](https://github.com/mcereal/mesh-client/commit/d208be9132fa7d26e0cf109dd74721e52d360e57))
* **firmware:** offer nRF52 BLE DFU only on BlueZ, and resume only for -p ([b59ff81](https://github.com/mcereal/mesh-client/commit/b59ff81291f3b01d074a05dcc2b10c8a1d92f7ba))
* **firmware:** pace nRF52 DFU packets from each send, not a schedule ([329be07](https://github.com/mcereal/mesh-client/commit/329be076694b0210f6fb05a074ef1f914193a068))
* **firmware:** say why a download failed instead of repeating it ([dfb6c2d](https://github.com/mcereal/mesh-client/commit/dfb6c2df200f3e50d9d923ab13b77c189b34eef4))
* **firmware:** update UF2 fuzz harness for inkwell ([84d216d](https://github.com/mcereal/mesh-client/commit/84d216db4315f2033c64a6d21695b1c1632a6ffb))
* **firmware:** wait for Bluetooth before arming a BLE install ([278e62a](https://github.com/mcereal/mesh-client/commit/278e62a047f69ee253d134512d11eb7ca3c743be))
* **firmware:** wait out any Bluetooth refusal; CLI honours the staging variable ([535bb60](https://github.com/mcereal/mesh-client/commit/535bb60f11672528bfddf844be307e6d39ee3963))
* handle first-run Heltec V4 setup safely ([b54728c](https://github.com/mcereal/mesh-client/commit/b54728c006ea00a210c17805bbf62601033703fc))
* **map:** measure the attribution and the node labels ([04917ce](https://github.com/mcereal/mesh-client/commit/04917cef0fa105898e9f7886d0798c0bb6e00901))
* **nav:** hand the snackbar to the next notice when a press dismisses one ([2dd3ee4](https://github.com/mcereal/mesh-client/commit/2dd3ee4435f7881f5ce69d14666857248e95cc40))
* **nav:** pin inkstand to the snackbar that waits out the press (59b01ed) ([ec095aa](https://github.com/mcereal/mesh-client/commit/ec095aa971e782a54c46ac6ab8b8eea0891efdf6))
* **net:** a broker refusal must not log as a success ([52221b9](https://github.com/mcereal/mesh-client/commit/52221b9245f8bd59c301b3680faab73c511e52a4))
* **overlays:** declare the copy, and keep help the topmost thing ([041653e](https://github.com/mcereal/mesh-client/commit/041653e303f6a4bc634cc689bf2d1388bba23df3))
* **release:** close a running client before the Windows uninstall ([e361eaa](https://github.com/mcereal/mesh-client/commit/e361eaaf55995d36484795b36f50e92aaa0e47ca))
* **release:** delete each rpath once; expect a binary on macOS in the updater test ([05eb271](https://github.com/mcereal/mesh-client/commit/05eb2717a6ffa023ffda8bae36ebb1af38ad6e15))
* **serial:** wait for the bind's tty on the tick, not in the connect ([6470cb0](https://github.com/mcereal/mesh-client/commit/6470cb039638f3327b18135acd74122359882625)), closes [mcereal/inkwell#18](https://github.com/mcereal/inkwell/issues/18)
* **settings:** check a field id against the table before narrowing it ([e19f250](https://github.com/mcereal/mesh-client/commit/e19f25085387e4c80bdcf04d382bbd481f345692))
* **settings:** keep the stepper when a segmented control falls back to a word ([39496a5](https://github.com/mcereal/mesh-client/commit/39496a5c2b1b11bd16052124af331ae2586928e0))
* **settings:** track reboots across handshake resets ([c04c74e](https://github.com/mcereal/mesh-client/commit/c04c74ef6fe181cc2c9f244499ed006c05d9ec93))
* setup-macos syncs stale submodules; AGENTS.md names the Mac path ([17a0834](https://github.com/mcereal/mesh-client/commit/17a083437474da5549777f8d77f0b20b278bf09a)), closes [inkcell#28](https://github.com/mcereal/inkcell/issues/28)
* **setup:** bootstrap WSL without make ([318a097](https://github.com/mcereal/mesh-client/commit/318a0970b6b69a7a95589d5feef6b351f1c87e3a))
* **setup:** support Windows worktrees via WSL ([52d8e96](https://github.com/mcereal/mesh-client/commit/52d8e96ee0df7e9b66907b9aa26a40ff464bfd8e))
* **store:** say when the trend logs of a swapped radio could not be dropped ([f95944b](https://github.com/mcereal/mesh-client/commit/f95944b3754d0b6a5ef44869a99f4f97a6d9ec10))
* **ui:** a backend that will not open falls back, from review ([7ff14ba](https://github.com/mcereal/mesh-client/commit/7ff14babd6d15c49af207cadb0c1d871dc6a5e81))
* **ui:** a click stands armed deletes down and waits for a current frame ([32369f6](https://github.com/mcereal/mesh-client/commit/32369f6f4128961c36fede27c1e0e8a85aade629))
* **ui:** a frame request joins the one already armed ([3c28e83](https://github.com/mcereal/mesh-client/commit/3c28e83e7925045ba2a53f7adaec253147eb5520))
* **ui:** dismiss context menu before host text ([34b0c4d](https://github.com/mcereal/mesh-client/commit/34b0c4d6c6276cda80abf1194fb13b4965f985b3))
* **ui:** drop a remote node's pending edits when leaving it from any tab ([4397bb8](https://github.com/mcereal/mesh-client/commit/4397bb8f8b5c2675e732417cb74e6733ee6e1df6))
* **ui:** find the open thread's row in the list beside it by what it is ([9e17d36](https://github.com/mcereal/mesh-client/commit/9e17d36036da2370872b582b9c0b9f1d31d356d5))
* **ui:** keep a list's cursor-row verbs off the pointer heading ([26dcf08](https://github.com/mcereal/mesh-client/commit/26dcf08cb5197572c626fde6033c75981952cd32))
* **ui:** keep the places cursor when the Nodes tab is not showing ([f57bcba](https://github.com/mcereal/mesh-client/commit/f57bcba18e5011f38c588b3b4888884fe361c4dc))
* **ui:** make the Status cards' buttons clickable ([3ddab42](https://github.com/mcereal/mesh-client/commit/3ddab426b17de3af7e3860cff5729b3846de061d))
* **ui:** mark a node row's distance approximate when either fix is rounded ([708f7fc](https://github.com/mcereal/mesh-client/commit/708f7fc456af296b732706d2e84a4dfbf673027f))
* **ui:** never draw a screen transition under the partial-redraw band ([adc7059](https://github.com/mcereal/mesh-client/commit/adc7059a366aea8c8591645cf4f789d957879b4c))
* **ui:** new data under a right-click menu puts it down ([dd45473](https://github.com/mcereal/mesh-client/commit/dd4547329ea1272b245f52078f07a202e4239c57))
* **ui:** pin inkcell so a focused card verb clears the card's edge ([f9b19cc](https://github.com/mcereal/mesh-client/commit/f9b19cc3bb99fe7d4604dcfd1462c31c9ebfaf4e))
* **ui:** pin inkcell so help's back arrow takes a click (89b206d) ([1f56690](https://github.com/mcereal/mesh-client/commit/1f566902158b0a2a7adfeb36a65ddd531302509f)), closes [#57](https://github.com/mcereal/mesh-client/issues/57)
* **ui:** pin inkcell so the focus ring jumps between Radio tab cards (77b63a3) ([aaf9f43](https://github.com/mcereal/mesh-client/commit/aaf9f43d7e7c85a89764e5d713c145930839705a))
* **ui:** pin inkcell to the review fix for the card verb clearance (207776b) ([d0ea245](https://github.com/mcereal/mesh-client/commit/d0ea2455fec853793afcb13df0cd8617aa2460cd))
* **ui:** preserve context menu order ([9cceeb7](https://github.com/mcereal/mesh-client/commit/9cceeb77e8e0169ea53f56f03f9588fe2a278a27))
* **ui:** take the slide offset from the scaffold's transition after the merge ([5a2e416](https://github.com/mcereal/mesh-client/commit/5a2e416c39550c44ae63bdb6d537ca32c3dfead9))
* **ui:** the control socket removes nothing it did not create ([a0a001b](https://github.com/mcereal/mesh-client/commit/a0a001b5cfecf1fd31b26b1315ec99a7c9289664))
* **ui:** three headers the shim sweep took that were not shims ([e3e1fab](https://github.com/mcereal/mesh-client/commit/e3e1fab02b53b5acbc8a758d21e908a91982412d))
* **update:** offer no macOS update where the bundle's folder is read-only ([3c97467](https://github.com/mcereal/mesh-client/commit/3c97467ac84eecad2a431c125388ee1d296e0b49))
* **update:** replace the whole bundle on macOS; UTF-8 code page on Windows ([66ce9d2](https://github.com/mcereal/mesh-client/commit/66ce9d2da81474154783cf414e41b6cbc3394547))
* **windows:** check the client's printf-like formats against the CRT's dialect ([5d249ef](https://github.com/mcereal/mesh-client/commit/5d249efa085b78e75829a31833662d1431345d1a))
* **windows:** fail setup check for stale submodules ([f547f1a](https://github.com/mcereal/mesh-client/commit/f547f1ab75b5a6026bdf2757f0ce61ca9a840045))
* **windows:** honor configured SDL default for auto backend ([cf82143](https://github.com/mcereal/mesh-client/commit/cf82143552ea6d3362fe713aa7c95430a000169d))
* **windows:** make resolver lifecycle nonblocking ([340bb3d](https://github.com/mcereal/mesh-client/commit/340bb3dce46ab2893226ae370e176eda0bb87d1d))

### Documentation

* **actions:** why mesh_ui_action_bar_goes_back() is this client's ([603c9dd](https://github.com/mcereal/mesh-client/commit/603c9dde56db9096e3462937a8784a882baaa355)), closes [mcereal/inkcell#23](https://github.com/mcereal/inkcell/issues/23)
* correct the map after the inkcell extraction ([f26799f](https://github.com/mcereal/mesh-client/commit/f26799ffb652f0392ad158b136cff765a43481c9)), closes [#include](https://github.com/mcereal/mesh-client/issues/include)
* re-render the listing screenshots ([5a6e599](https://github.com/mcereal/mesh-client/commit/5a6e599a3aefaf530c17b0e2dabffe3c568f8200))
* re-render the listing stills for the one-row foot ([933ad30](https://github.com/mcereal/mesh-client/commit/933ad3067a7b83b7be9f790b40a771901c0e4180))
* the paths the extraction cleanup missed ([60bb611](https://github.com/mcereal/mesh-client/commit/60bb611a56dc20a5aca9baf896e221b0665e409d)), closes [#258](https://github.com/mcereal/mesh-client/issues/258) [#258](https://github.com/mcereal/mesh-client/issues/258)
* **transport:** say that inkwell depends on this port's VMIN ([398f5de](https://github.com/mcereal/mesh-client/commit/398f5de1d7a7168e372859141e237867f9a126f9))
* **ui:** the list roles and the one-row footer; refresh the listing stills ([4eadad5](https://github.com/mcereal/mesh-client/commit/4eadad514f4c1382d84f385c29b7cada2b717ad8))
* **windows:** plaintext MQTT works; TLS and HTTPS fetch remain ([8d66cb1](https://github.com/mcereal/mesh-client/commit/8d66cb19ea7dfcc80fed4ee95ad97c55442586d5))

### Code Refactoring

* **app:** drive the control socket through a host, not the controller ([86af729](https://github.com/mcereal/mesh-client/commit/86af7294b18acb7304061ca961930b98aa837486))
* **app:** use inkstand's control socket ([9965abf](https://github.com/mcereal/mesh-client/commit/9965abf5fcf84a7e8085592c066dcec1cf6b502e))
* **ble:** stand the BLE transport on inkwell's central ([5f72f80](https://github.com/mcereal/mesh-client/commit/5f72f80d048d4431fbec5b42062f6e38d6a208d4)), closes [mcereal/inkwell#11](https://github.com/mcereal/inkwell/issues/11)
* **ble:** use inkwell connection intervals ([08685ca](https://github.com/mcereal/mesh-client/commit/08685ca4d4605d08663df14860e71bcfa06bc523))
* **devtools:** play ui_capture's scenes on inkstand's runner ([8b5ff3c](https://github.com/mcereal/mesh-client/commit/8b5ff3cdbc9772fee8b20681eb391d9f8bdbacbe)), closes [#317](https://github.com/mcereal/mesh-client/issues/317)
* **devtools:** split the scene runner out of ui_capture's main.c ([c611e3d](https://github.com/mcereal/mesh-client/commit/c611e3d8e74c236805a1490cc3f3ae9d2127f466))
* **firmware:** fetch the image through inkwell's net/zip_fetch.h ([725bbcf](https://github.com/mcereal/mesh-client/commit/725bbcf98c2cca6d803af66feaa5c2bc6f9db58d))
* **firmware:** use inkwell image codecs ([07f1e79](https://github.com/mcereal/mesh-client/commit/07f1e79611cac4be7f4b0c105f8ecfdd313ca0b0))
* **firmware:** use inkwell USB storage ([ed6fbd9](https://github.com/mcereal/mesh-client/commit/ed6fbd94826545321ef2db526fe07a5c198af43b))
* give inkwell the five things here that were never about a mesh ([f462d1e](https://github.com/mcereal/mesh-client/commit/f462d1e5ed2c28e0e175f90baa46d921822f47dd))
* **i18n:** word TCP and MQTT failures through mesh_net_reason_format() ([3b26681](https://github.com/mcereal/mesh-client/commit/3b26681a94793903e46e85d053cec24e7ac4536b))
* **keyboard:** the grid is inkcell's; what it is for stays here ([c85f511](https://github.com/mcereal/mesh-client/commit/c85f5111b401d020241a891dc31a42e1aff7cfda))
* **mqtt:** use inkwell client ([21aa6ca](https://github.com/mcereal/mesh-client/commit/21aa6caadcea1694971e9131669c560f5dd4d78d))
* **nav:** answer the confirm sheet through inkstand's nav/dialog.h ([469b598](https://github.com/mcereal/mesh-client/commit/469b598df55af2184297f11dbf896cc68d3a2832))
* **nav:** keep the snackbar's queue through inkstand's nav/toast.h ([41c5c42](https://github.com/mcereal/mesh-client/commit/41c5c424c3f23587a715643386413371b3a0433d))
* **nav:** split the snackbar's queue from the nav ([8247c7b](https://github.com/mcereal/mesh-client/commit/8247c7baf0e6ece4fd28bea9e1a1cf1c2acc16dd))
* **nav:** split the two-answer dialog from the confirm sheet ([04e5ba1](https://github.com/mcereal/mesh-client/commit/04e5ba15fddf967202e5fa744cf50eb31e7b1ca7))
* **net,ui:** the MQTT proxy reports a reason, not a sentence ([a78379e](https://github.com/mcereal/mesh-client/commit/a78379e92daeb74ca1dfa96bc38eb1da92fa48f1))
* **net:** the TLS client is handed its roots and the fetcher its name ([8d747af](https://github.com/mcereal/mesh-client/commit/8d747afa8f70bcdd6eff8b1b0e86c1f1770a59ab))
* **net:** the TLS session and the HTTPS request are inkwell's ([1d17cb8](https://github.com/mcereal/mesh-client/commit/1d17cb875f22c93093098e67752d25fbf70d4805))
* **serial:** take the USB serial ports from inkwell's io/serial.h ([be3401d](https://github.com/mcereal/mesh-client/commit/be3401d3b03baa53b7ac9ba1084162416c3a2009)), closes [mcereal/inkwell#18](https://github.com/mcereal/inkwell/issues/18)
* **settings:** describe every field through inkstand's form descriptor ([33327da](https://github.com/mcereal/mesh-client/commit/33327daefa921cd5f69039d4fe83fdffd247b3cd))
* **settings:** parse and print settings through inkstand's form codec ([75d9041](https://github.com/mcereal/mesh-client/commit/75d904177fc6698464fc80bc6f28e30f3b0d12f1))
* **settings:** split the field descriptor from this client's field table ([ad88a7c](https://github.com/mcereal/mesh-client/commit/ad88a7c31270a0e7f83b918819b712c44cd31006))
* **settings:** split the number scale and the choice set from the field table ([de14ec5](https://github.com/mcereal/mesh-client/commit/de14ec56eb22d8ada1246431c54b60a57f3389c4))
* **settings:** split the text codec from this client's spellings of it ([e8c19ed](https://github.com/mcereal/mesh-client/commit/e8c19ed103195130ad3b9942ed2172b1b244eb2b))
* **settings:** step and place presets through inkstand's form scale ([d34869d](https://github.com/mcereal/mesh-client/commit/d34869de36a018bc144a2c4a86df1d996bf2605c))
* stand on inkwell ([c40dadb](https://github.com/mcereal/mesh-client/commit/c40dadbdf7e34e283791962a93a997468928d9b7))
* **store:** keep both logs through inkstand's journal ([66065f0](https://github.com/mcereal/mesh-client/commit/66065f0f15ea4d84bc56f3177fa4be6c777971d1))
* **store:** keep the preferences' lists through inkstand's recently-used list ([b4e0590](https://github.com/mcereal/mesh-client/commit/b4e0590576564f7b6ef47a563fad583aacfea0dd))
* **store:** one journal under the archive and the trend log ([503286b](https://github.com/mcereal/mesh-client/commit/503286b4ca961bd74c10a3550b46d6aa088ea111))
* **store:** one recently-used list under both of the preferences' lists ([8becb2a](https://github.com/mcereal/mesh-client/commit/8becb2aa1f9be57a947b0af0219a44612ab65ecd))
* **store:** read the cache through inkstand's key table and field reader ([8ae7802](https://github.com/mcereal/mesh-client/commit/8ae7802d1888a0a0a0304067fc28cabd344e6b35))
* **store:** split the key-table mechanism from the cache's keys ([84a2981](https://github.com/mcereal/mesh-client/commit/84a2981b1334b79153cc3ae573035220f2a5d3c0))
* **store:** use inkwell record file primitives ([7ea0ca5](https://github.com/mcereal/mesh-client/commit/7ea0ca56bfc68289149ef37a195b27e36368f449))
* **tcp:** use inkwell connector ([b3d6e4f](https://github.com/mcereal/mesh-client/commit/b3d6e4f60667c3549e03a9dc56ff3b808d41faf4))
* **transport,i18n:** the TCP link reports a reason, not a sentence ([03a900d](https://github.com/mcereal/mesh-client/commit/03a900d17cfb301fce3d1571202598d35acd8fde))
* **transport:** the stream link is the parser and the session over inkwell's stream ([4273690](https://github.com/mcereal/mesh-client/commit/4273690b216062f4640406b9f866e02fa8d87f6e))
* **ui:** declare commands in action tables ([9693dc4](https://github.com/mcereal/mesh-client/commit/9693dc4aec3ad56d7ecae215543f0b4832c43898))
* **ui:** dispatch context menu commands ([75cde8d](https://github.com/mcereal/mesh-client/commit/75cde8df77adc56e5cb7630b8b696e95bf78515f))
* **ui:** dispatch semantic commands ([cc9c0bb](https://github.com/mcereal/mesh-client/commit/cc9c0bb87aaec8dbd39c86fcba5f871dd01daf4b))
* **ui:** drop the path shims and include the inkcell headers ([83e1f6b](https://github.com/mcereal/mesh-client/commit/83e1f6bdf49c697b12161f90587da23a9ed5b6b9))
* **ui:** follow inkcell's drawing state off the framebuffer ([7c987f9](https://github.com/mcereal/mesh-client/commit/7c987f9cce95a775efcd82b410a7067c7972cba4))
* **ui:** hold a settings field id in 16 bits wherever it is kept ([0e7c957](https://github.com/mcereal/mesh-client/commit/0e7c9574ebf00fe1695e154db530e3e03225af1e))
* **ui:** introduce semantic command foundation ([58f7f28](https://github.com/mcereal/mesh-client/commit/58f7f28e3a71087da662af6652163a22c01471cc))
* **ui:** present through inkstand's frame scheduler ([4c7fab0](https://github.com/mcereal/mesh-client/commit/4c7fab05bd67f3bfd2713795abad386b9e8cc8a0))
* **ui:** retire inkcell_compat.h and spell the inkcell names ([ae03228](https://github.com/mcereal/mesh-client/commit/ae03228e17c97fa727af1944c892a88dd6246580)), closes [#defined](https://github.com/mcereal/mesh-client/issues/defined)
* **ui:** split the frame scheduler out of the controller ([b411e47](https://github.com/mcereal/mesh-client/commit/b411e4794d7ec82b7275558cdd491a0f3b7f9731))

## [2.71.1](https://github.com/mcereal/mesh-client/compare/v2.71.0...v2.71.1) (2026-09-20)

### Bug Fixes

* build clean under clang, which the sanitizer job uses ([bde2791](https://github.com/mcereal/mesh-client/commit/bde2791521370ca288e75a4b42602f24ba3aa69b))
* two more places the environment prefix and the catalog were missed ([2208abf](https://github.com/mcereal/mesh-client/commit/2208abf214e18ac2f613f69de2b516d70725b2a0))

### Code Refactoring

* build the UI on inkcell instead of carrying it ([3cee07c](https://github.com/mcereal/mesh-client/commit/3cee07c3b0de1f55f1fe95cf61cefcba237c4328))

## [2.71.0](https://github.com/mcereal/mesh-client/compare/v2.70.0...v2.71.0) (2026-09-20)

### Features

* **admin:** configure another node's radio over the mesh ([c105868](https://github.com/mcereal/mesh-client/commit/c105868ec6a0a0257cffeab87653a1b3c7bf105f))
* **channels:** share and import a channel set as a Meshtastic link ([3eba831](https://github.com/mcereal/mesh-client/commit/3eba831bcdad4a128c7b681fce9de207281fa7e2))
* **contacts:** share and add a contact as a Meshtastic link ([4eb8cbf](https://github.com/mcereal/mesh-client/commit/4eb8cbfeaa6f64f76b6bb0313c394bc7559957e2))
* **core:** make the padlock mean something with key trust ([ab784b5](https://github.com/mcereal/mesh-client/commit/ab784b553042d55abd70b5207c18d9c99f2faa62))
* **messages:** keep a transcript per conversation on the card ([0ced5e5](https://github.com/mcereal/mesh-client/commit/0ced5e50c48407135e49077a186f0de44a0a09cb))
* **mqtt:** proxy for a radio that asks the client to hold its broker connection ([1bac50c](https://github.com/mcereal/mesh-client/commit/1bac50ca5812f2807cab4345312600d763a0ab45))
* **mqtt:** say on the Status screen whether the broker connection is working ([23caae4](https://github.com/mcereal/mesh-client/commit/23caae4a5cc43633634b92c759f2876bc7761e00))
* **net:** fetch over HTTPS in-process instead of forking curl ([b1409c4](https://github.com/mcereal/mesh-client/commit/b1409c4eace6010ca092ac9fa839435f8e6fc19f))
* **proto:** an HTTP/1.1 codec for fetching in-process ([e55a2a4](https://github.com/mcereal/mesh-client/commit/e55a2a408d9c6bd75b0002e5679042899fb42ac3))
* **settings:** a row that is one bit of a field, and the checkbox it draws ([819bec3](https://github.com/mcereal/mesh-client/commit/819bec398f781e9ec4f74d54c3ab0b389d800307))
* **settings:** LoRa's advanced group, and the ham mode that licenses it ([aea76a9](https://github.com/mcereal/mesh-client/commit/aea76a9a358927e8b75939c8d896f6dd78610593))
* **settings:** make "Proxy via client" editable, and put it where it belongs ([68aee06](https://github.com/mcereal/mesh-client/commit/68aee06ce4b49634b4f505d323c3d59f5539b501))
* **settings:** mesh beacon, and the four copies of one record it needed ([a9572f0](https://github.com/mcereal/mesh-client/commit/a9572f0ef85ff83bf490f769a8b09e0dd92e8541))
* **settings:** offer Display's six unsurfaced fields, and record the protobuf gap audit ([349e8c1](https://github.com/mcereal/mesh-client/commit/349e8c1f2fabd91a99c59411f568519818fae5ec))
* **settings:** read the network, the mute and what the firmware was built without ([1cefef8](https://github.com/mcereal/mesh-client/commit/1cefef813baac1232e5dec7598202acf957b7b5a))
* **settings:** the presets a region will take, and the row that says so ([8389a52](https://github.com/mcereal/mesh-client/commit/8389a52a4c63b29f37faf0cb9705d5cdfb15706f))
* **store-forward:** identify a replayed message by original_id ([b495c84](https://github.com/mcereal/mesh-client/commit/b495c84bfc9c6d253c3df48925970647e2e40ccb))
* **tls:** verify against CA roots compiled into the binary ([cd9b6eb](https://github.com/mcereal/mesh-client/commit/cd9b6eb74266daf46545afc98f1eab4a96c72447))
* **transport:** accept a hostname for --tcp-host ([457b4d8](https://github.com/mcereal/mesh-client/commit/457b4d8fa8654dcb5911b54db47d80a758b01e87))
* **ui:** a node's chart lists its readings as well as drawing them ([22d8428](https://github.com/mcereal/mesh-client/commit/22d8428688847b8b33cadf15cb7f23b93ead70ca))
* **ui:** clear a channel slot rather than only disabling it ([fde8b23](https://github.com/mcereal/mesh-client/commit/fde8b230f06b7a5c490abbeb33b6d051e07c46a7))
* **ui:** draw a settings verb as a verb, and a section as cards ([47bd2ea](https://github.com/mcereal/mesh-client/commit/47bd2ea46043d542c2172c09c66f671f3cd55cd8))
* **ui:** filter the Nodes list with a chip row ([a4c5df4](https://github.com/mcereal/mesh-client/commit/a4c5df409fb03581a3942234fa64b0c76b4c6b66))
* **ui:** give the keyboard the pad's own buttons, and an emoji layer ([f0610dc](https://github.com/mcereal/mesh-client/commit/f0610dc40ae989ce7e690ffb6ce0d5f0ec473f7e))
* **ui:** give the node detail colour where a phone app puts it ([e410c7d](https://github.com/mcereal/mesh-client/commit/e410c7d1833ac9ceb0d1c9adbc66004d92b6595c))
* **ui:** keep a measured route per node, on the card ([358901e](https://github.com/mcereal/mesh-client/commit/358901e61961f4aaefa9ad6612ac3d02860e59bd))
* **ui:** keep a node's signal-to-noise and received strength over time ([4d092f5](https://github.com/mcereal/mesh-client/commit/4d092f550b8802eaf072011b02abe32f75db0fcf))
* **ui:** keep a node's trends on the card across a restart ([eea0632](https://github.com/mcereal/mesh-client/commit/eea06325cc492b32e5da475de1c9c3ce452a769f))
* **ui:** make the Nodes filter and sort read as controls ([e7d9e6f](https://github.com/mcereal/mesh-client/commit/e7d9e6f44003715b836fd02918d57ddbd08f86cd))
* **ui:** open a node's detail on the node, not on its verbs ([cc5d903](https://github.com/mcereal/mesh-client/commit/cc5d903da0dcf99ed05f7bd4ac64df9b3dfebf87))
* **ui:** say who relayed a packet and where it goes next ([ee84b95](https://github.com/mcereal/mesh-client/commit/ee84b95ca5dc34a48e7a0ea5809230933e75597b))
* **ui:** send a failed message again from the bubble it failed on ([da78f05](https://github.com/mcereal/mesh-client/commit/da78f058eacacbc412ec9e5c47aa8301d4899f91))
* **ui:** sort the Nodes list by last heard, name, distance or hops ([10cbb20](https://github.com/mcereal/mesh-client/commit/10cbb20cd0f3dfdd0c28e7347f82f3364b99d26b))
* **ui:** type a radio's network address on the Devices tab ([3fee02c](https://github.com/mcereal/mesh-client/commit/3fee02c0a752b5887b71227bb6f316048d5eec58))
* **ui:** Up and Down step a node detail by card, not by row ([504d496](https://github.com/mcereal/mesh-client/commit/504d496232bba8a333573179c97b2fafba9242d3))

### Bug Fixes

* **admin:** keep a remote request's reply the only reply it gets ([e2791ae](https://github.com/mcereal/mesh-client/commit/e2791ae03cadd7dba4d27dadfe88e8755898cacb))
* **app:** arm the long post-drop grace only for the preferred radio's link ([ad4db91](https://github.com/mcereal/mesh-client/commit/ad4db9115194cc1049c7b042d007352e03a60fa5))
* **app:** drop a bond its own firmware install invalidated ([da2ff52](https://github.com/mcereal/mesh-client/commit/da2ff526f0669223b4eb85e181dc790c545c6ef7))
* **app:** only judge a bond the bus was free to test ([abc3daf](https://github.com/mcereal/mesh-client/commit/abc3daff8d787f98459fab13cb70522f80ba1052))
* **app:** report a queued DFU request as an armed radio ([b91ba4a](https://github.com/mcereal/mesh-client/commit/b91ba4abae3a934e26dff0422eb267a1f7659b20))
* **app:** take short loop turns while a handover has the radio ([9ccb1c2](https://github.com/mcereal/mesh-client/commit/9ccb1c20325a8c69c4d7d93f2cf30c4b1d74d5c6))
* **app:** wait the long grace for a radio that just dropped ([b56c851](https://github.com/mcereal/mesh-client/commit/b56c8515dd749fbdf3e81382bf33640efcbf8cf7))
* **channels:** wait for a settled table, and skip a LoRa write that changes nothing ([336ab7f](https://github.com/mcereal/mesh-client/commit/336ab7f5a406435db7e29cc49332bdff9892706d))
* **cli:** name the channel a resolved release came from ([6862e5c](https://github.com/mcereal/mesh-client/commit/6862e5cc706e9bde0ae619168727a329748e4b5d))
* **cli:** resolve firmware on the configured channel, not always stable ([681b4ef](https://github.com/mcereal/mesh-client/commit/681b4ef9379671e5671879f4a4ce499b309491d4))
* **contacts:** put an imported contact in the roster, and name the node the write lands on ([3a9b2e3](https://github.com/mcereal/mesh-client/commit/3a9b2e3856214c4a3c8a368cf95f361d5b16e9ba))
* **contacts:** reach the roster through the store, not nav_internal.h ([3c1d785](https://github.com/mcereal/mesh-client/commit/3c1d785992fd0002114eab65dd4616005396234d))
* **core:** four ways the ceremony kept the wrong half of a pair ([fead914](https://github.com/mcereal/mesh-client/commit/fead914039f176817507acdd9e4df88fba71d701)), closes [#182](https://github.com/mcereal/mesh-client/issues/182)
* **core:** remember only the network address the transport adopted ([c3659a0](https://github.com/mcereal/mesh-client/commit/c3659a00571bc278bd7c4fc1dd4264b451d891a2)), closes [#179](https://github.com/mcereal/mesh-client/issues/179)
* **devtools:** point the Nodes scenes at the rows they name ([ed74afb](https://github.com/mcereal/mesh-client/commit/ed74afbce29a6d460f8144fe16ca9df81ab072d0))
* **firmware:** connect to the OTA loader before dropping the scan ([bb0cd09](https://github.com/mcereal/mesh-client/commit/bb0cd09d5e4e3140fc797155b052fcf6a99d0682))
* **firmware:** keep the state a finished fetch left behind ([af35a13](https://github.com/mcereal/mesh-client/commit/af35a13621ffdaaf8be5f51e3c61d0df45fed1de))
* **firmware:** refuse a zip member bigger than any radio's flash ([85aba0e](https://github.com/mcereal/mesh-client/commit/85aba0e171f4bd4f9239b4c5b137a6a1a6959be0))
* **i18n:** a label the row cannot hold is a label nothing says was cut ([46a99ae](https://github.com/mcereal/mesh-client/commit/46a99ae983398840d460f1578973b91349416129))
* **input:** let the device filter see the trigger axes ([36ddc1e](https://github.com/mcereal/mesh-client/commit/36ddc1e86188cd6d4ddcad8454cce2a52240a3eb))
* **log:** bound the log on the card and quiet the scan that filled it ([bc3360b](https://github.com/mcereal/mesh-client/commit/bc3360b04393de56aff3086c43252c2bc38c8f56))
* **log:** only derive a log path from a pak's userdata directory ([cf5cc7d](https://github.com/mcereal/mesh-client/commit/cf5cc7d43f5480bcaefad7fba409334dae032042))
* **log:** resolve the log's path once when cutting it back ([b668059](https://github.com/mcereal/mesh-client/commit/b668059e3a2b8691996e8071bf94ed3c52a1ab28))
* **messages:** make a packet id name one message, and one file survive a delete ([94c8728](https://github.com/mcereal/mesh-client/commit/94c8728905f397ac74010cdfa77a3ee1c8d635b5))
* **mqtt:** subscribe once to a repeated channel id ([bc104ae](https://github.com/mcereal/mesh-client/commit/bc104aec9020045aa17f8122499d7fda7f73428e))
* **net:** keep a chained request's lookup when the last one is freed ([028b2e9](https://github.com/mcereal/mesh-client/commit/028b2e91d466ef3d498229cdfd2160eb0abd8003))
* **net:** try a host's other addresses when one will not connect ([a5ed138](https://github.com/mcereal/mesh-client/commit/a5ed13849dc4a98d633d8368c4092f92892460e9))
* point each published build's self-update at its own asset ([9174f45](https://github.com/mcereal/mesh-client/commit/9174f454bb8e16fd67c7282d9ad6c157aff94186)), closes [#ifndef](https://github.com/mcereal/mesh-client/issues/ifndef)
* **proto:** resolve a fragment-only Location to the base URL ([9d91246](https://github.com/mcereal/mesh-client/commit/9d912464a69939aad13881c3aef44a1fb88895f8))
* read angle-bracket includes in the layering check ([5ba4a12](https://github.com/mcereal/mesh-client/commit/5ba4a122f5855b419479649e9a5f1cf51decf2d8)), closes [#include](https://github.com/mcereal/mesh-client/issues/include)
* **session:** drop a link whose radio has stopped answering ([9f05b36](https://github.com/mcereal/mesh-client/commit/9f05b36363e2e08e5f85d8d338323a29672138c5))
* **settings:** the preset map goes with the link, and an unknown licence is not "no" ([f226a0e](https://github.com/mcereal/mesh-client/commit/f226a0e1b1befbc32f0326e70d6f242bd85790a2))
* **settings:** three ways the new LoRa rows read the wrong thing ([27e8537](https://github.com/mcereal/mesh-client/commit/27e853726a12d308f67c189d598d64ce64d4a19f))
* **settings:** two beacon rules stated on a screen and enforced nowhere ([cc3fe39](https://github.com/mcereal/mesh-client/commit/cc3fe39adf0569d2eaa467950b354fd4aaaf826f)), closes [#190](https://github.com/mcereal/mesh-client/issues/190)
* **status:** take the ceremony's question off the Radio card once it is answered ([b8d3c5f](https://github.com/mcereal/mesh-client/commit/b8d3c5f580653c3d441e1187f0f7bd2a5863ec8e))
* **store-forward:** keep a replayed message out of the notices ([928c2f8](https://github.com/mcereal/mesh-client/commit/928c2f8ec48b4510da3bcc71149ceb94b09c5dfb))
* **tests:** include text.h for mesh_str_copy in the settings suite ([fcc1779](https://github.com/mcereal/mesh-client/commit/fcc1779eab3fdaa82ae7c5096078ae6d6b1857da))
* **tests:** keep the BLE rig's event loop alive across a restart ([be07137](https://github.com/mcereal/mesh-client/commit/be07137f055c5398c54eb82e9ca60d1e1105c89f))
* **tests:** read the preset row through the public accessor ([e4a2e82](https://github.com/mcereal/mesh-client/commit/e4a2e82786ea9de86d38bc2cc6cdbe327fe8ac36))
* **tls:** bound the session-ticket retry per read ([5161e56](https://github.com/mcereal/mesh-client/commit/5161e5689bfaacb9d661b0e8cb06e6d5e5216d97))
* **tls:** drop a partly loaded root set instead of keeping it ([22325d2](https://github.com/mcereal/mesh-client/commit/22325d2f6df017f92f54547a1e1f8f774108c800))
* **tls:** read past a TLS 1.3 session ticket ([8e94bab](https://github.com/mcereal/mesh-client/commit/8e94bab577962bff49e0f351a1655e5f62558aa1))
* **ui:** a card does not spend its padding into a full row ([cb68fb1](https://github.com/mcereal/mesh-client/commit/cb68fb115f613b9dd4ad9a6efadbdccb11efde43))
* **ui:** a card's edge lives outside both boxes it separates ([6e7c868](https://github.com/mcereal/mesh-client/commit/6e7c8688922d00f27326d440086b1d6bb6d9aa53))
* **ui:** a settings chevron is a promise the nav keeps ([361fffd](https://github.com/mcereal/mesh-client/commit/361fffd331f318c2c80a86f84e1de086b4749c6e))
* **ui:** an excluded section advertises no press that needs rows ([113c82b](https://github.com/mcereal/mesh-client/commit/113c82bf63f47fe867182f57073a6e89cdc062a7))
* **ui:** ask one question about whether a section has groups ([56624c0](https://github.com/mcereal/mesh-client/commit/56624c0fd55e3bf30479e155e4652bd643d9bf97))
* **ui:** ask the clear row about every field the clear erases ([67738ee](https://github.com/mcereal/mesh-client/commit/67738eea19b937cf829f001497d9a9bb3c3b196a))
* **ui:** ask the trend log's descriptor, not its path ([3d824ac](https://github.com/mcereal/mesh-client/commit/3d824ac736c9b9e2deb50d490371bc6165a9141f))
* **ui:** bound the Nodes filter set against the segmented button ([83fa493](https://github.com/mcereal/mesh-client/commit/83fa4939f216df48ef2babc705e8279e225f440d))
* **ui:** chart the radio's airtime per minute, as columns ([218f436](https://github.com/mcereal/mesh-client/commit/218f436297112585bda1f1c68cf5058653ea8937))
* **ui:** commit a disc and a chip to the family base on the cursor ([ebbfc03](https://github.com/mcereal/mesh-client/commit/ebbfc0387e12f27f6a0d3c3af70a6523f3e007e0))
* **ui:** convert an altitude to feet without overflowing an int ([282d26b](https://github.com/mcereal/mesh-client/commit/282d26bc6a2e66e93ed1f634c8e05461c19da165))
* **ui:** declare mesh_ui_node_our_fix where nav_waypoints.c calls it ([b334f45](https://github.com/mcereal/mesh-client/commit/b334f456ef4119816ca148d5ed23f501ae2c2f38))
* **ui:** do not name a relay the roster cannot pin down ([7de2df6](https://github.com/mcereal/mesh-client/commit/7de2df679cb94cc8f82ad6075a9959c8052e812b))
* **ui:** draw an emoji box wider than the column map ([c2bcee1](https://github.com/mcereal/mesh-client/commit/c2bcee1fa6aa894f342bfcb290d098254ee6474f))
* **ui:** draw the keyboard's emoji at the size of the key ([9a2543a](https://github.com/mcereal/mesh-client/commit/9a2543a8960e5ee806cda5e3362c917a08d4387f))
* **ui:** four faults in the trend log, found in review ([ae43e02](https://github.com/mcereal/mesh-client/commit/ae43e02f381bd0d00dcb03993c864a64644ecc2d))
* **ui:** guard the cache index against a 32-bit unsigned long ([c9fd71a](https://github.com/mcereal/mesh-client/commit/c9fd71ab60bfa9c19f46fb46a683e0cbb7b86032))
* **ui:** head and card every settings group the same way ([9e3a97f](https://github.com/mcereal/mesh-client/commit/9e3a97f76cc2d7e3c00e8ffd2c6828289474813f))
* **ui:** keep a card heading's disc clear of the cards either side ([e1c5385](https://github.com/mcereal/mesh-client/commit/e1c5385eff6774aa1f34e8b497364bca43609d0d))
* **ui:** keep a failed message's reason on the card ([a3a96ea](https://github.com/mcereal/mesh-client/commit/a3a96ea6320da461706144494a1d7ac206e1cefd))
* **ui:** key the signal trend on the measurement, not the arrival ([6e609ec](https://github.com/mcereal/mesh-client/commit/6e609ec79ee5e7282249fd4dbd1432ad48928369))
* **ui:** leave the tab switch alone on an empty Nodes list ([0c69a1f](https://github.com/mcereal/mesh-client/commit/0c69a1f1cc52d880993d710342ce523bc8a474d6))
* **ui:** let the sheet of verbs pass on the presses it does not own ([527a261](https://github.com/mcereal/mesh-client/commit/527a261bdcaa14404035f4c0cf7175fb755990d9))
* **ui:** lift the airtime line over a silence inside wide bins ([342ff7b](https://github.com/mcereal/mesh-client/commit/342ff7bba4f62da13873121227d132a04734ade4))
* **ui:** make a card a grouping, and a verb's colour its own ([7561fe9](https://github.com/mcereal/mesh-client/commit/7561fe92fb9ce664c050c79821ca4edec1bd109e))
* **ui:** make the retry one send per press ([6fb722b](https://github.com/mcereal/mesh-client/commit/6fb722bc68b34e3b0019000a03ca9a9312496830))
* **ui:** match the Nodes filters to what the list draws ([1ebb3ce](https://github.com/mcereal/mesh-client/commit/1ebb3ce695354e39a6d6300f5685134ed452bd43))
* **ui:** one rule each for the disc, the gutter and a row's two tiers ([492cc62](https://github.com/mcereal/mesh-client/commit/492cc621ce8b80ecbc3d5be004c99e44d79dd7f1))
* **ui:** page the facts of a node card that also holds a press ([508ee56](https://github.com/mcereal/mesh-client/commit/508ee5617decd2805a66be6a16e9c670ea0ec8ba))
* **ui:** read the action bar's row off this node's route ([a498f22](https://github.com/mcereal/mesh-client/commit/a498f2219ad05d5fe27836faaa6483c9dfa0882d))
* **ui:** read the radio's units everywhere a length is shown ([b471afc](https://github.com/mcereal/mesh-client/commit/b471afc7f146dd7a43e40661e670d4524c2e99ed))
* **ui:** settings rows start their words in one column ([d848a58](https://github.com/mcereal/mesh-client/commit/d848a58da974621f3c22cc4b34d4bd3f2d88ef21))
* **ui:** stand a mixed group's verbs on the panel, not on a second card ([ea51f66](https://github.com/mcereal/mesh-client/commit/ea51f66113d4804d400019658e3d8228b8ca7030))
* **ui:** write the handshake cache through a temporary ([ae687f9](https://github.com/mcereal/mesh-client/commit/ae687f9a0e938002b6f495d1039ffdf60c15832a))
* **verify:** ask a question the user can answer when the radio sends no code ([2d15f0b](https://github.com/mcereal/mesh-client/commit/2d15f0b13d229338a2b7be765229ab62d1d1c4a9))
* **verify:** put the waiting sheet back on every verify press ([b130062](https://github.com/mcereal/mesh-client/commit/b1300622473dd715cee9da1d306ebd02f9c7e4b3))
* **verify:** take the six-digit security number the firmware reads out ([4029384](https://github.com/mcereal/mesh-client/commit/40293844706149f1f249e8b076d42d7f8c6423dd))

### Documentation

* compress the non-bugs list onto the tests that hold it ([b450908](https://github.com/mcereal/mesh-client/commit/b45090896641376aaafd3bfdbd9af1cf524caba1))
* cut docs/ from 14.4k lines to 2.5k ([9b885b3](https://github.com/mcereal/mesh-client/commit/9b885b3efa3257aa65b0aecf6414d0dea96928ae))
* drop three more citations no test actually holds ([c92a320](https://github.com/mcereal/mesh-client/commit/c92a3203c3bd4b0a01003c277af688ae0945a7ac))
* fix two references the cleanup missed ([d5c1eab](https://github.com/mcereal/mesh-client/commit/d5c1eab84927354780998af58b3d58210d9da0a9))
* gate the non-bugs list on evidence rather than on reasoning ([1932f8d](https://github.com/mcereal/mesh-client/commit/1932f8d8ffdbafa808c6eba905933ba890242b46))
* make the tested-first sort in non-bugs.md actually hold ([d935613](https://github.com/mcereal/mesh-client/commit/d9356137a2661c4b9bc19fff3c25e93317614de4))
* move the rule list out of CLAUDE.md into docs/non-bugs.md ([423c316](https://github.com/mcereal/mesh-client/commit/423c316a1f4722349d837eddc01e09fa2f60fef2))
* section non-bugs.md and compress the entries a test holds ([d145a23](https://github.com/mcereal/mesh-client/commit/d145a23f7e41a3aee148453dc9add64cf2c1b3a8))
* **ui:** the card-edge rule, as it ended up ([d2ebeb0](https://github.com/mcereal/mesh-client/commit/d2ebeb061b38b71e0d1b0e709816460701f720fe))

### Code Refactoring

* **app:** split mesh_app_on_ui_action into a handler per verb ([75bece3](https://github.com/mcereal/mesh-client/commit/75bece3d5563945272c8f59c67d9d3ffafca5757))
* file src/ui and src/core by group ([f4783e9](https://github.com/mcereal/mesh-client/commit/f4783e9be9b9d044b0ce30435113daf572e84238)), closes [#include](https://github.com/mcereal/mesh-client/issues/include)
* **firmware:** inflate a zip member in-process instead of forking gzip ([53d74e6](https://github.com/mcereal/mesh-client/commit/53d74e6d3ffeb3062afc7c898105adbe96d077e7))
* make the layering a rule the build checks ([2148090](https://github.com/mcereal/mesh-client/commit/214809001f9740cf6e58c46b90892e5c485b7b07)), closes [#include](https://github.com/mcereal/mesh-client/issues/include)
* split fb_screens into one file per screen ([574386f](https://github.com/mcereal/mesh-client/commit/574386ff884e573094edee33cf00c2edc62e03ac))
* split fb_widgets into one file per group of components ([331ed88](https://github.com/mcereal/mesh-client/commit/331ed88e633b2ff414ee4af31825253d03ffb371))
* **tests:** collapse the case epilogue and app $HOME setup onto one idiom ([1860336](https://github.com/mcereal/mesh-client/commit/18603365cb0fa1b4b637978071c1624445aa5794))
* **tests:** promote the BLE transport rig to tests/support ([0e4e174](https://github.com/mcereal/mesh-client/commit/0e4e174c4a4455ea370944e3a6630fbf8778f647))
* **ui:** a node's trends come out of a shared pool ([dc46b9a](https://github.com/mcereal/mesh-client/commit/dc46b9a649d0ac6161cf831bcfa9db95d16b5870))
* **ui:** read a cache value through a field list ([1824579](https://github.com/mcereal/mesh-client/commit/1824579f9d2bad1fdd71a8b6a048a01e97253a49))
* **ui:** split store.h into six subject headers ([3704e9a](https://github.com/mcereal/mesh-client/commit/3704e9a9c08f46cb9872d133c34ede475dcb867e))
* **ui:** the cache format's keys, spelled once instead of three times ([3b2fc37](https://github.com/mcereal/mesh-client/commit/3b2fc37188dc015f4b23d24030073f4c49ee49e0))

## [2.70.0](https://github.com/mcereal/mesh-client/compare/v2.69.2...v2.70.0) (2026-09-13)

### Features

* **ui:** measure a press to the panel, and close step 3 of the map ([5c1db2d](https://github.com/mcereal/mesh-client/commit/5c1db2da39e1b2fdfdb39bae33c2d1ceec7e938f))
* **ui:** walk the node detail by card, and stop promising presses it does not have ([09ee3f4](https://github.com/mcereal/mesh-client/commit/09ee3f48941c7a52921b39733dcc6673d8211bbd))

### Bug Fixes

* **ui:** time only presses that changed the frame, and stop the client on a failed run ([dc76066](https://github.com/mcereal/mesh-client/commit/dc760667d8fd65b4a04748a338282582384a4ce0)), closes [#177](https://github.com/mcereal/mesh-client/issues/177)

### Documentation

* assess other handhelds against what the client actually requires ([e13dfa2](https://github.com/mcereal/mesh-client/commit/e13dfa2e928a0a52500b2820e4f93c046445bd13)), closes [#else](https://github.com/mcereal/mesh-client/issues/else)
* **maps:** say plainly that the synthetic pack is a fixture, not a style ([a7f9fb5](https://github.com/mcereal/mesh-client/commit/a7f9fb568e2e8624a2c4b3d3a30a0f7f7ffd741e))
* name the serial driver allowlist rather than implying there is none ([5d42531](https://github.com/mcereal/mesh-client/commit/5d425319794a178107ce8a1f732af52c24c37c3c))

## [2.69.2](https://github.com/mcereal/mesh-client/compare/v2.69.1...v2.69.2) (2026-09-13)

### Bug Fixes

* **ui:** stop a read conversation rewriting the cache on every update ([3f6e314](https://github.com/mcereal/mesh-client/commit/3f6e314b4fea1e91fe512631738b748548520078))

## [2.69.1](https://github.com/mcereal/mesh-client/compare/v2.69.0...v2.69.1) (2026-09-13)

### Bug Fixes

* **ui:** fill the keyboard's tenth symbol key ([fc42ef4](https://github.com/mcereal/mesh-client/commit/fc42ef40b37dfd6d1b3b086206ea1c88476487f2))

## [2.69.0](https://github.com/mcereal/mesh-client/compare/v2.68.8...v2.69.0) (2026-09-12)

### Features

* **crash:** leave a readable report behind when the client faults ([8478566](https://github.com/mcereal/mesh-client/commit/84785668f077536f52b53fc623c12bb76c9a5140))

### Bug Fixes

* **crash:** stop the report promising privacy it cannot deliver, and survive an exhausted stack ([6592d8f](https://github.com/mcereal/mesh-client/commit/6592d8f260bf28b3f1c41d4994a324e7d4da48ba))

## [2.68.8](https://github.com/mcereal/mesh-client/compare/v2.68.7...v2.68.8) (2026-09-12)

### Bug Fixes

* **ui:** draw a row's label and its value as two tiers ([02030c1](https://github.com/mcereal/mesh-client/commit/02030c174e30647412ce1481151fcefb61c09275))
* **ui:** let a row's label inherit its tone ([7eeecab](https://github.com/mcereal/mesh-client/commit/7eeecab9a8708a32c6ec828afd84a722621f8c60))

### Documentation

* **ui:** stop naming the helper the headline no longer has ([6360dcd](https://github.com/mcereal/mesh-client/commit/6360dcd2b063785edeea5970f9beb97d0ecb0de7))

## [2.68.7](https://github.com/mcereal/mesh-client/compare/v2.68.6...v2.68.7) (2026-09-12)

### Bug Fixes

* **ui:** keep the radio's airtime trend across a restart ([1296598](https://github.com/mcereal/mesh-client/commit/1296598d752b65dfb5a604bccf98ffaa0b43459b))

## [2.68.6](https://github.com/mcereal/mesh-client/compare/v2.68.5...v2.68.6) (2026-09-12)

### Bug Fixes

* **ui:** give the scroll rail a gutter of its own beside a list's cards ([d7af8c5](https://github.com/mcereal/mesh-client/commit/d7af8c5879393da4a823e7472031728d29914494))

## [2.68.5](https://github.com/mcereal/mesh-client/compare/v2.68.4...v2.68.5) (2026-09-12)

### Bug Fixes

* **ui:** widen the framebuffer size multiply before it overflows ([7f6ed1a](https://github.com/mcereal/mesh-client/commit/7f6ed1aec0606b1fb45c03668bd1eed82c3a98fc))

## [2.68.4](https://github.com/mcereal/mesh-client/compare/v2.68.3...v2.68.4) (2026-09-12)

### Bug Fixes

* **ui:** give the node detail's cards room to be cards ([e5e3ad2](https://github.com/mcereal/mesh-client/commit/e5e3ad243e6e405ff751afac0e642ae9ca8c8fb0))
* **ui:** keep a card's inset out of a heading's room at the smallest scale ([2ee1de6](https://github.com/mcereal/mesh-client/commit/2ee1de6880b147c9b000313508168cb1d8568a63))

## [2.68.3](https://github.com/mcereal/mesh-client/compare/v2.68.2...v2.68.3) (2026-09-12)

### Bug Fixes

* **ui:** say why an assetless release cannot be installed ([673a4a7](https://github.com/mcereal/mesh-client/commit/673a4a7e28b36aa1f8a89e53342201b7a3a6ebae))
* **ui:** stop an up-to-date radio reporting the installer as unbuilt ([b66ed1b](https://github.com/mcereal/mesh-client/commit/b66ed1ba5213c26580bf0c51475a7d40b3ddfae4))

## [2.68.2](https://github.com/mcereal/mesh-client/compare/v2.68.1...v2.68.2) (2026-09-12)

### Bug Fixes

* **ui:** give the airtime trend a gap the radio's own cadence fits in ([18f9a69](https://github.com/mcereal/mesh-client/commit/18f9a69a2785ef4b5b269839406b829349a65330))

## [2.68.1](https://github.com/mcereal/mesh-client/compare/v2.68.0...v2.68.1) (2026-09-12)

### Bug Fixes

* **waypoint:** keep the header's assertions spellable from C++ ([b414da7](https://github.com/mcereal/mesh-client/commit/b414da7d86ef97f1555d94384066557125d66a95))
* **waypoint:** state the name and description limits as characters, not buffers ([af3bcc4](https://github.com/mcereal/mesh-client/commit/af3bcc4b88c593cce590be8c4691fb92756b7654))

## [2.68.0](https://github.com/mcereal/mesh-client/compare/v2.67.1...v2.68.0) (2026-09-12)

### Features

* **ui:** give the charts a span picker, a contracting ceiling and one renderer ([3bdb91f](https://github.com/mcereal/mesh-client/commit/3bdb91fccd9cf7b29c27fc07fbc41f60fe3870ae))

### Bug Fixes

* **ui:** pick a chart's ceiling from the readings it draws ([b6f4b68](https://github.com/mcereal/mesh-client/commit/b6f4b6871f27144dc27447f7eb9812430c0dbada))

## [2.67.1](https://github.com/mcereal/mesh-client/compare/v2.67.0...v2.67.1) (2026-09-12)

### Bug Fixes

* **build:** clear every compiler warning the build, the pak and CI emit ([e816320](https://github.com/mcereal/mesh-client/commit/e8163207471c8f179003ebcce61235bdc4cb3af8))
* **build:** settle the libc branch inside the ioctl header, and make it a helper ([638df88](https://github.com/mcereal/mesh-client/commit/638df8826d4727ede9996beec40aebc794e01ba8)), closes [#161](https://github.com/mcereal/mesh-client/issues/161)

## [2.67.0](https://github.com/mcereal/mesh-client/compare/v2.66.0...v2.67.0) (2026-09-12)

### Features

* **ui:** draw the node detail's groups as cards ([cde4442](https://github.com/mcereal/mesh-client/commit/cde4442b80ceecc32a9637e7b57c832cd3834b2e))

### Bug Fixes

* **ui:** give a list control the row's resting ground, not its current one ([186920e](https://github.com/mcereal/mesh-client/commit/186920edc02a4c8a97e4e40422840f5efb176d94))
* **ui:** keep a card heading in the list's own leading gutter ([7836cb9](https://github.com/mcereal/mesh-client/commit/7836cb9485cde31eaa53e9ba522011136b906520))
* **ui:** keep the traced route out of the node detail's action block ([05d2b6e](https://github.com/mcereal/mesh-client/commit/05d2b6eeb0fcf9020531b2b4db3faa96525d226e))

## [2.66.0](https://github.com/mcereal/mesh-client/compare/v2.65.0...v2.66.0) (2026-09-12)

### Features

* **settings:** measure the edit buffer from the field table ([84e0aa2](https://github.com/mcereal/mesh-client/commit/84e0aa29d196caa3e58896b772b74db415d4c4c4))

## [2.65.0](https://github.com/mcereal/mesh-client/compare/v2.64.0...v2.65.0) (2026-09-12)

### Features

* **ui:** bring the node detail up to the component set ([9a34278](https://github.com/mcereal/mesh-client/commit/9a342783b28cbd03040f18ca0be236fd699796f0))

### Bug Fixes

* **ui:** open the detail on a real row from the map, and key its switches by node ([6ec75da](https://github.com/mcereal/mesh-client/commit/6ec75dad7bce607e44a4661ec743fb50f1ca3038)), closes [#158](https://github.com/mcereal/mesh-client/issues/158)

## [2.64.0](https://github.com/mcereal/mesh-client/compare/v2.63.1...v2.64.0) (2026-09-12)

### Features

* **firmware:** install the radio's firmware from Settings ([d159342](https://github.com/mcereal/mesh-client/commit/d159342a1cf70a697bbdb2ab7316ba103de2eec1))

### Bug Fixes

* **firmware:** revalidate the radio, refuse TCP, and let the loader be recovered from ([274f301](https://github.com/mcereal/mesh-client/commit/274f301ccf8459a508a63b78eca977b3485b4759)), closes [#157](https://github.com/mcereal/mesh-client/issues/157)

## [2.63.1](https://github.com/mcereal/mesh-client/compare/v2.63.0...v2.63.1) (2026-09-11)

### Bug Fixes

* **map:** bound a direction's reach to one step so the map can be explored ([cbfeecb](https://github.com/mcereal/mesh-client/commit/cbfeecb8c40ffc0175d163ceb55966c5abf92638))

## [2.63.0](https://github.com/mcereal/mesh-client/compare/v2.62.0...v2.63.0) (2026-09-11)

### Features

* **devtools:** draw a tile pack, and a scene that reads one ([6ab0581](https://github.com/mcereal/mesh-client/commit/6ab05819eb0776e791a5209f8625d37be884da1a))
* **map:** draw the basemap under the markers ([7c14810](https://github.com/mcereal/mesh-client/commit/7c1481036102223e6ad458ffe99a6550d3e7d305))

### Bug Fixes

* **devtools:** wrap a synthetic pack's columns, and make its output directory ([9ba894d](https://github.com/mcereal/mesh-client/commit/9ba894ddbb027fcc15f6e1ffef26f4dc1b8ee253)), closes [#155](https://github.com/mcereal/mesh-client/issues/155)

### Documentation

* **map:** record what the fill loop and the blit turned out to be ([4ebd943](https://github.com/mcereal/mesh-client/commit/4ebd9438e5637d9a6e69f27ad2117923994d744c))

## [2.62.0](https://github.com/mcereal/mesh-client/compare/v2.61.1...v2.62.0) (2026-09-11)

### Features

* **map:** keep decoded tiles, and remember which ones are not there ([5b03d06](https://github.com/mcereal/mesh-client/commit/5b03d06323bffb3fa416c67a63ccbb70779287fa))

### Bug Fixes

* **map:** keep a hole when a tile claim is refused ([d384144](https://github.com/mcereal/mesh-client/commit/d384144e142a48b130bcfc7ffd2d00c8799d1a2f))

### Documentation

* correct three claims in the Steam Deck notes ([4cd9d97](https://github.com/mcereal/mesh-client/commit/4cd9d97b33afc407dec54d2cac1d696673da5eba))
* what running mesh-client on a Steam Deck takes ([69c4ea6](https://github.com/mcereal/mesh-client/commit/69c4ea653f0d1236dc7f5569baeeef5098e51241))

## [2.61.1](https://github.com/mcereal/mesh-client/compare/v2.61.0...v2.61.1) (2026-09-11)

### Bug Fixes

* **devtools:** count the Map row when a scene walks the Nodes list ([a7c98bf](https://github.com/mcereal/mesh-client/commit/a7c98bf7f8ef0bdc0400a310c639c31f40ef3b10))

## [2.61.0](https://github.com/mcereal/mesh-client/compare/v2.60.0...v2.61.0) (2026-09-11)

### Features

* **map:** decode a tile, with Wuffs vendored ([81d0511](https://github.com/mcereal/mesh-client/commit/81d05115d0fe5390b8022378143a486729912c68))

### Documentation

* **map:** say that a tile damaged after its last pixel still decodes ([c4f3b50](https://github.com/mcereal/mesh-client/commit/c4f3b50eef3f64ac43f1c38591883fc25bec66b0))

## [2.60.0](https://github.com/mcereal/mesh-client/compare/v2.59.0...v2.60.0) (2026-09-11)

### Features

* **map:** read a tile pack, and say which tiles a view stands on ([895d95c](https://github.com/mcereal/mesh-client/commit/895d95ce451b9e79ad64349bebb6cbfda21cd475))

### Bug Fixes

* **map:** zero a refused source, and diagnose a short pack index ([fbfaf2a](https://github.com/mcereal/mesh-client/commit/fbfaf2a91d67793a08b978143220e02415e7b0ea))

### Documentation

* **maps:** record that pans cost the same in both directions ([27016f4](https://github.com/mcereal/mesh-client/commit/27016f4e85f5c93b6f7c65940a41d4c9eeed0e18))
* **maps:** record what the Brick measured for step 3 ([8bb9c8a](https://github.com/mcereal/mesh-client/commit/8bb9c8a41290cf21ca09312462f6facc1a05d374))

## [2.59.0](https://github.com/mcereal/mesh-client/compare/v2.58.0...v2.59.0) (2026-09-11)

### Features

* **ui:** chart a node's temperature and humidity over time ([db4d095](https://github.com/mcereal/mesh-client/commit/db4d095bd40a3f5c780c815be67e70bf66a007b4))

### Bug Fixes

* **ui:** close a node's chart with its row, and need a line to open one ([21c2969](https://github.com/mcereal/mesh-client/commit/21c29693961c6f1c7f2888ae1c6d5f3b3aa1379f))

## [2.58.0](https://github.com/mcereal/mesh-client/compare/v2.57.0...v2.58.0) (2026-09-11)

### Features

* **firmware:** install ESP32 radio firmware over Bluetooth ([dc3c01b](https://github.com/mcereal/mesh-client/commit/dc3c01b8eebce20e08e71091595ae9e2891b09bf))

### Bug Fixes

* **firmware:** arm only the named radio, and only behind an empty admin queue ([1a7208a](https://github.com/mcereal/mesh-client/commit/1a7208a5a9075393fb1a8694e9d4e0c743861dbb))

## [2.57.0](https://github.com/mcereal/mesh-client/compare/v2.56.1...v2.57.0) (2026-09-11)

### Features

* **input:** make the pad and the panel table-driven ([5b5fcd5](https://github.com/mcereal/mesh-client/commit/5b5fcd51b6b874854cbaeb9136ab1a698e64aa3d))

### Bug Fixes

* **input:** bind the xbox profile's X and Y by code, not by compass name ([77dfda1](https://github.com/mcereal/mesh-client/commit/77dfda1a77d587063fcb80a0f1d2942bc225c1a8))

## [2.56.1](https://github.com/mcereal/mesh-client/compare/v2.56.0...v2.56.1) (2026-09-11)

### Bug Fixes

* **app:** route a network row's press to the network transport ([8e3195d](https://github.com/mcereal/mesh-client/commit/8e3195d024487f411503ddb3298ce9e60e570861))
* **tcp:** adopt a connect's host as the one to go back to ([899a665](https://github.com/mcereal/mesh-client/commit/899a665df6f3bb5ea1cb7506fe35fbe48d42288f))

## [2.56.0](https://github.com/mcereal/mesh-client/compare/v2.55.0...v2.56.0) (2026-09-11)

### Features

* **transport:** reach a Meshtastic node over the network ([e5d127c](https://github.com/mcereal/mesh-client/commit/e5d127cce527e6182297ec1f8c095de88bd89de0))

### Bug Fixes

* **transport:** four findings from review ([c0c0b52](https://github.com/mcereal/mesh-client/commit/c0c0b527d92312954e12c733c4bf2b00af62ae56))
* **ui:** a network link is not a Bluetooth radio ([ddcadc7](https://github.com/mcereal/mesh-client/commit/ddcadc7da7b849f89eaa0ad7e8774ed38e249b49))

### Documentation

* **firmware:** answer phase 0's BLE throughput question ([6601d01](https://github.com/mcereal/mesh-client/commit/6601d01c58a18c24cbb403ecba2615c3a99be998))
* **firmware:** size a chunk from the write payload, not the MTU ([a227952](https://github.com/mcereal/mesh-client/commit/a227952700ceab515711c1c182d5900bacdbb3f8))

### Code Refactoring

* **transport:** lift the shared half out of the serial link ([f4b881d](https://github.com/mcereal/mesh-client/commit/f4b881d49c848f25468445f3ef71c53d7c36b030))

## [2.55.0](https://github.com/mcereal/mesh-client/compare/v2.54.1...v2.55.0) (2026-09-10)

### Features

* **messages:** make an arriving message visible from anywhere ([7c58e93](https://github.com/mcereal/mesh-client/commit/7c58e9341f5f3e4ceeeffd8b4d3a17450150331e))

### Bug Fixes

* **messages:** three notification bugs from review ([9237f3a](https://github.com/mcereal/mesh-client/commit/9237f3a30ca17430e0c969de5c2024bdbc513e82))

## [2.54.1](https://github.com/mcereal/mesh-client/compare/v2.54.0...v2.54.1) (2026-09-10)

### Bug Fixes

* **firmware:** hold the drive, and read a write that ended as a restart ([6713140](https://github.com/mcereal/mesh-client/commit/6713140776dc8466bf8977967019b5f59d410a6d))
* **firmware:** open the drive without testing it first ([2029852](https://github.com/mcereal/mesh-client/commit/20298525df37da9090dea502dc3e59d3206f9f1b))

## [2.54.0](https://github.com/mcereal/mesh-client/compare/v2.53.0...v2.54.0) (2026-09-10)

### Features

* **deploy:** add USB (adb) transport to the device deploy loop ([317b996](https://github.com/mcereal/mesh-client/commit/317b996a59e6a1afd0ba8afac4624720b48708b5))

## [2.53.0](https://github.com/mcereal/mesh-client/compare/v2.52.0...v2.53.0) (2026-09-10)

### Features

* **firmware:** ask a radio to enter its UF2 bootloader ([1a2833a](https://github.com/mcereal/mesh-client/commit/1a2833a96964f1a5baff00d3f2ed1eb7fac41e5c))
* **firmware:** write a UF2 to a radio's bootloader over USB ([2143699](https://github.com/mcereal/mesh-client/commit/2143699d6ab74107af445327ef0bf43785c32874))

### Bug Fixes

* **deploy:** compress the push stream ([c565f06](https://github.com/mcereal/mesh-client/commit/c565f06b1ede3880f09f77e050b2bd06963830b3))
* **deploy:** stage a push under a name the launcher does not glob ([9ad77f2](https://github.com/mcereal/mesh-client/commit/9ad77f2192985e1b8ea1074a8388c60ee247fcf5))
* **firmware:** fill in why an install refused to start ([61824f1](https://github.com/mcereal/mesh-client/commit/61824f13c01b1cce17940c3bb7dcb33d96271c1d))
* **firmware:** match the bootloader on the bus id, not on the tty label ([c5e047d](https://github.com/mcereal/mesh-client/commit/c5e047dd509c56706647ba8ec055b0f10f230726))

### Documentation

* **firmware:** mark phase 3 shipped and correct its test plan ([957934e](https://github.com/mcereal/mesh-client/commit/957934ea2061c7d4eaf6823a4e26f9c31fd2ceef))
* **firmware:** say plainly that phase 3 is unconfirmed on hardware ([d78515c](https://github.com/mcereal/mesh-client/commit/d78515cbb2182778f0f66ca452b46c32144e413f))

## [2.52.0](https://github.com/mcereal/mesh-client/compare/v2.51.1...v2.52.0) (2026-09-10)

### Features

* **firmware:** fetch one board's image out of a release zip ([0b9eea4](https://github.com/mcereal/mesh-client/commit/0b9eea481f5ea85fe85a8f212c898a65940d5858))
* **firmware:** read a release zip from its back end ([f39c149](https://github.com/mcereal/mesh-client/commit/f39c14988f7e1fd4769aaf11816e4bdadabe49dd))
* **firmware:** resolve a board to its image and fetch it ([877d390](https://github.com/mcereal/mesh-client/commit/877d39055ae6b25d5bc65a680564fd97038eee18))

### Bug Fixes

* **firmware:** check the manifest is for the board we asked for ([cd2913a](https://github.com/mcereal/mesh-client/commit/cd2913a225cd4e21d1fcb40f066593c6f99b8d8d)), closes [#137](https://github.com/mcereal/mesh-client/issues/137)

### Documentation

* **firmware:** mark phase 2 shipped and correct four numbers ([8674f7c](https://github.com/mcereal/mesh-client/commit/8674f7cda195522b3d205a97de98ccd986f066d1))

## [2.51.1](https://github.com/mcereal/mesh-client/compare/v2.51.0...v2.51.1) (2026-09-10)

### Bug Fixes

* **cli:** do not pick a bootloader as the implicit serial target ([25574ea](https://github.com/mcereal/mesh-client/commit/25574ea503e5615e25655ffe3e9fec1b10608c7e))
* **serial:** stop calling a UF2 bootloader a radio ([1bca3d0](https://github.com/mcereal/mesh-client/commit/1bca3d0edd097c411389e78c618234f76cd8397a))

### Documentation

* **firmware:** answer phase 0's USB half on hardware ([7b9ef44](https://github.com/mcereal/mesh-client/commit/7b9ef44cba494074c3e76c9a229d4781e71735d9))

## [2.51.0](https://github.com/mcereal/mesh-client/compare/v2.50.0...v2.51.0) (2026-09-10)

### Features

* **ui:** key the Status cursor on a verb rather than a position ([eac21e1](https://github.com/mcereal/mesh-client/commit/eac21e10768387b9ed3f0eb83d792e1a53e2ee64))

## [2.50.0](https://github.com/mcereal/mesh-client/compare/v2.49.0...v2.50.0) (2026-09-10)

### Features

* **ui:** say each Status reading once and give the room to the card in trouble ([d0a3974](https://github.com/mcereal/mesh-client/commit/d0a3974665afc0a1c727892b59d5598f101a7b78))

### Bug Fixes

* **ui:** measure the Status card's shares 64 bits wide ([6db5a8e](https://github.com/mcereal/mesh-client/commit/6db5a8e8549d632e2660c87bef70d4757e855b30))

## [2.49.0](https://github.com/mcereal/mesh-client/compare/v2.48.0...v2.49.0) (2026-09-10)

### Features

* **ui:** draw the airtime trend as a chart with its axes named ([0620378](https://github.com/mcereal/mesh-client/commit/062037888f5c8ee177c790a8a0f42144f9409934))
* **ui:** measure several series on one clock window ([41a5be3](https://github.com/mcereal/mesh-client/commit/41a5be341544fd647bebdcf38999ef010c08033f))

### Bug Fixes

* **ui:** offer a trend only when there is a line to draw ([2444d8a](https://github.com/mcereal/mesh-client/commit/2444d8a97d4538985f5a2a06cb95b2e77acd49c9))

### Code Refactoring

* **ui:** say how long ago and how long for in one place ([95536d4](https://github.com/mcereal/mesh-client/commit/95536d42156d0e421a4f362cb203c5479e79f20c))

## [2.48.0](https://github.com/mcereal/mesh-client/compare/v2.47.1...v2.48.0) (2026-09-10)

### Features

* **ui:** add a proportion bar and the categorical palette it needs ([4b857d3](https://github.com/mcereal/mesh-client/commit/4b857d340a3493994958fbf757c9c704bfb2e02e))

### Bug Fixes

* **ui:** draw a proportion bar's gaps in the ground it is actually on ([cbf40ff](https://github.com/mcereal/mesh-client/commit/cbf40ffb034788540b8cff79beb7331478fea715))
* **ui:** reserve room for the last card in a column ([158203c](https://github.com/mcereal/mesh-client/commit/158203cd2c45a275ff03620ea2bac3d2019e7c9f))

## [2.47.1](https://github.com/mcereal/mesh-client/compare/v2.47.0...v2.47.1) (2026-09-10)

### Bug Fixes

* **map:** step past the marker the crosshair is already on ([cfd8074](https://github.com/mcereal/mesh-client/commit/cfd8074802dc7a80929b51cf1d9a3762b13dba50))
* **map:** step to the next marker instead of panning onto a lattice ([77371bc](https://github.com/mcereal/mesh-client/commit/77371bc80cad18766306f8c5643fc91686a65941))

## [2.47.0](https://github.com/mcereal/mesh-client/compare/v2.46.0...v2.47.0) (2026-09-10)

### Features

* **settings:** offer the radio firmware channel as a row ([57bb8e9](https://github.com/mcereal/mesh-client/commit/57bb8e97314c338be54f0f5f649c73c3a5f1078b))

### Documentation

* **maps:** correct the offline premise and re-sequence around acquisition ([086d1e3](https://github.com/mcereal/mesh-client/commit/086d1e37b6d6d6328a28fd3bbfb53b610ac4c964))
* **maps:** name the right viewport inverse for an on-screen download ([2fe6656](https://github.com/mcereal/mesh-client/commit/2fe66565c8cfeb040aefcee8b485941d5145ea85))

## [2.46.0](https://github.com/mcereal/mesh-client/compare/v2.45.0...v2.46.0) (2026-09-10)

### Features

* **core:** read the firmware catalog upstream publishes ([d8b824f](https://github.com/mcereal/mesh-client/commit/d8b824fc3edb2879109385c59e76811e4ee13aba))
* **settings:** say what firmware the radio could be running ([593a31f](https://github.com/mcereal/mesh-client/commit/593a31fb2aa6aca42c5a750acf4deb2b96279ef6))

### Bug Fixes

* **core:** drop a firmware answer that belongs to another radio ([014f7bd](https://github.com/mcereal/mesh-client/commit/014f7bd2e76e5e18b0faaacac2020b4320724a5d))
* **core:** refuse a release whose tag is a 'v' and nothing else ([4e90f4d](https://github.com/mcereal/mesh-client/commit/4e90f4df40fef88f05af668dc27ec7d7958c9fba))
* two findings from the Codex review ([e505b1e](https://github.com/mcereal/mesh-client/commit/e505b1eda725780d265592bf2b545000214efb45))

### Code Refactoring

* **core:** lift the forked fetcher out of the updater ([d26cc75](https://github.com/mcereal/mesh-client/commit/d26cc75bd9f02705ff4d47f16dfc2c0eb10d1de2))

## [2.45.0](https://github.com/mcereal/mesh-client/compare/v2.44.0...v2.45.0) (2026-09-10)

### Features

* **map:** draw every positioned node the session holds ([fefdb4f](https://github.com/mcereal/mesh-client/commit/fefdb4f349258fd1e32d4dc3affde6495c9bbd9b))

### Bug Fixes

* **app:** auto-connect to the radio you have, not the one BlueZ remembers ([d4ba6bd](https://github.com/mcereal/mesh-client/commit/d4ba6bdb9770d521f98cf7bfb97f22db7f8bf6fb))
* **app:** give the range test a scan to read, and the grace a reconnect ([8cce1f6](https://github.com/mcereal/mesh-client/commit/8cce1f6dfe9461eed2cae070b574f6de3755dc3e))

### Documentation

* add the USB/UF2 path and put it first ([37ed61b](https://github.com/mcereal/mesh-client/commit/37ed61b9cc0d185bc9e3a95cf5da014da568ec63))
* assess updating the radio's firmware from the Brick ([10f6dfc](https://github.com/mcereal/mesh-client/commit/10f6dfc3f9df483e9c323ab2540cfa5c98628a0c))
* correct how the map decides what the crosshair is on ([cbc35db](https://github.com/mcereal/mesh-client/commit/cbc35dbe4319f284dbe0b59ff9a16fb562edbb1a)), closes [#123](https://github.com/mcereal/mesh-client/issues/123)
* correct two claims in the radio firmware assessment ([54ef467](https://github.com/mcereal/mesh-client/commit/54ef467efca47ced80f9d437e7435bce13aee75a))

## [2.44.0](https://github.com/mcereal/mesh-client/compare/v2.43.1...v2.44.0) (2026-09-09)

### Features

* **geo:** project a coordinate onto a flat picture of the world ([7bb0aae](https://github.com/mcereal/mesh-client/commit/7bb0aaec2c8bdcf2d1f90b2e78c3d33edf3672bb))
* **ui:** draw the map, and record what steps 1 and 2 became ([4134af1](https://github.com/mcereal/mesh-client/commit/4134af1c65eeff00b14bb9fada2a7116556b4002))
* **ui:** put the nodes and the places on a map ([d8358ea](https://github.com/mcereal/mesh-client/commit/d8358ea8a2eb6c8d30bf64e04e7fc64c86e37686))

### Bug Fixes

* **ui:** keep the map's presses and its ink inside the map ([0f32df2](https://github.com/mcereal/mesh-client/commit/0f32df2f6ecb26f571592c56c5c50fdcf1749bba)), closes [#123](https://github.com/mcereal/mesh-client/issues/123)

## [2.43.1](https://github.com/mcereal/mesh-client/compare/v2.43.0...v2.43.1) (2026-09-09)

### Bug Fixes

* **ui:** date a press's toast by the clock driving the frames ([7c8828d](https://github.com/mcereal/mesh-client/commit/7c8828de4f30c04cf8771801eaf70e5aa3435e2b))
* **ui:** say why a new waypoint cannot be made ([bc1463a](https://github.com/mcereal/mesh-client/commit/bc1463a54caf3723891553a4b8d52d092f293a4b))

## [2.43.0](https://github.com/mcereal/mesh-client/compare/v2.42.0...v2.43.0) (2026-09-09)

### Features

* **ui:** explain the long tail of settings rows ([9062190](https://github.com/mcereal/mesh-client/commit/90621901533805348b2e09f3837713210aa37d12))

### Bug Fixes

* **ui:** stop a help topic falling back across a subheading ([563149e](https://github.com/mcereal/mesh-client/commit/563149ed7fd94e0792df2ad7257a842caffc5447))

## [2.42.0](https://github.com/mcereal/mesh-client/compare/v2.41.0...v2.42.0) (2026-09-09)

### Features

* **ui:** explain the features as well as the settings ([e7b10b2](https://github.com/mcereal/mesh-client/commit/e7b10b26ffce400dfc55f2e3c38d7819361d64b4))

### Bug Fixes

* **ui:** ask the route whether a settings section is what is on the panel ([eb94f4d](https://github.com/mcereal/mesh-client/commit/eb94f4d2b80fd16c2c2f4ab30d1b332f49b18402))

## [2.41.0](https://github.com/mcereal/mesh-client/compare/v2.40.0...v2.41.0) (2026-09-09)

### Features

* **ui:** explain settings in the client, on a help screen SELECT opens ([a819c85](https://github.com/mcereal/mesh-client/commit/a819c8592fa69098ccef7eebf036fc7b96d30883))

### Bug Fixes

* **ui:** make the help press and its keycap agree in every section state ([5b36300](https://github.com/mcereal/mesh-client/commit/5b36300fbfe351135be89556e9d42f96817b0fa7))

## [2.40.0](https://github.com/mcereal/mesh-client/compare/v2.39.0...v2.40.0) (2026-09-09)

### Features

* **store-forward:** ask a router for the messages the Brick missed ([e42c972](https://github.com/mcereal/mesh-client/commit/e42c97268fdbc6f2f88ee82e5a6023b9aa8dbaac))

### Bug Fixes

* **store-forward:** stop dating a replay by when it was fetched ([34b523c](https://github.com/mcereal/mesh-client/commit/34b523c97451424dab939750c19cb5ffe7b92136))

## [2.39.0](https://github.com/mcereal/mesh-client/compare/v2.38.0...v2.39.0) (2026-09-09)

### Features

* **messaging:** send reactions and threaded replies, not just read them ([10d1eab](https://github.com/mcereal/mesh-client/commit/10d1eabb2b6c5122d0e55efa6ad825de8374bc8c))

### Bug Fixes

* **ui:** the tapback picker owns the body, so no banner may shorten it ([b7d97f6](https://github.com/mcereal/mesh-client/commit/b7d97f61c8a30102b3b9571286e78d6c6ce90b44))

## [2.38.0](https://github.com/mcereal/mesh-client/compare/v2.37.0...v2.38.0) (2026-09-08)

### Features

* **waypoints:** list, share and withdraw the mesh's shared places ([fe72cb6](https://github.com/mcereal/mesh-client/commit/fe72cb6fdf50e37cd424454bfa493cbffc1d1a4a))

### Bug Fixes

* **waypoints:** honour a dated expiry, and let the places follow the radio ([06bdc3c](https://github.com/mcereal/mesh-client/commit/06bdc3c1689e1d0e94d82905273499b201836e3c))

## [2.37.0](https://github.com/mcereal/mesh-client/compare/v2.36.0...v2.37.0) (2026-09-08)

### Features

* **ui:** mark a message's delivery state instead of spelling it ([154f023](https://github.com/mcereal/mesh-client/commit/154f0238dc760e1418ee43c0bb53930887a89a98))

### Documentation

* **ui:** say what a delivery mark's word is actually for ([ebebe47](https://github.com/mcereal/mesh-client/commit/ebebe4735ea608564178c770f380ecb794bf40fd))

## [2.36.0](https://github.com/mcereal/mesh-client/compare/v2.35.0...v2.36.0) (2026-09-08)

### Features

* **status:** count the roster a running sync has delivered ([5f73ac1](https://github.com/mcereal/mesh-client/commit/5f73ac1dc83ff6bc9420d71ac6121fb91cbb9f6c))

### Bug Fixes

* **ble:** stop the client losing a sync it had already earned ([d361bb2](https://github.com/mcereal/mesh-client/commit/d361bb275f9fc2b33b7089a2990938279f2103a1))
* **settings:** gate the radio actions on the link, not on knowing the radio ([7ea06fd](https://github.com/mcereal/mesh-client/commit/7ea06fdb38114b72052fb387c84b9e3c163db541))

## [2.35.0](https://github.com/mcereal/mesh-client/compare/v2.34.3...v2.35.0) (2026-09-08)

### Features

* **settings:** the four radio verbs that are not a section ([9603e90](https://github.com/mcereal/mesh-client/commit/9603e90f782dad53491c98689c7777bd6f3a9898))

### Bug Fixes

* **settings:** never let a canned save delete a message it could not show ([13561fc](https://github.com/mcereal/mesh-client/commit/13561fc7efddb5414838ff1f39a7018daebcdbbe)), closes [#113](https://github.com/mcereal/mesh-client/issues/113)

## [2.34.3](https://github.com/mcereal/mesh-client/compare/v2.34.2...v2.34.3) (2026-09-08)

### Bug Fixes

* close the four truncation sites the device build reports ([8e84129](https://github.com/mcereal/mesh-client/commit/8e84129068b42c26f40dfe9f62556d98feaeaefc)), closes [#110](https://github.com/mcereal/mesh-client/issues/110)

## [2.34.2](https://github.com/mcereal/mesh-client/compare/v2.34.1...v2.34.2) (2026-09-08)

### Bug Fixes

* **device:** stop input-map leaking readers, and hold a held button together ([ccf80c5](https://github.com/mcereal/mesh-client/commit/ccf80c5986d6e68e1e888ba8102b9fdb7463b6f6)), closes [#109](https://github.com/mcereal/mesh-client/issues/109)
* **input:** stop the power button quitting the client ([142cd6e](https://github.com/mcereal/mesh-client/commit/142cd6e0d054e25db37734bcda798afe83ff49d9))

## [2.34.1](https://github.com/mcereal/mesh-client/compare/v2.34.0...v2.34.1) (2026-09-08)

### Bug Fixes

* **ble:** stop scanning while a link is up or being made ([48ec65a](https://github.com/mcereal/mesh-client/commit/48ec65acc6a1ccbf2b9110fa74fb49b17289ffa8))

## [2.34.0](https://github.com/mcereal/mesh-client/compare/v2.33.1...v2.34.0) (2026-09-08)

### Features

* **geo:** say what a fix is, how precise, and whose clock dated it ([0219095](https://github.com/mcereal/mesh-client/commit/02190953a21a5f8a8f95e42f5cbdc3b3ad9b497e))

### Bug Fixes

* **geo:** check the conversion before the coordinate, and don't date a ([2c9f741](https://github.com/mcereal/mesh-client/commit/2c9f741c8bd23a8baee9b47ac7b2b1de08170d3b)), closes [#107](https://github.com/mcereal/mesh-client/issues/107)

## [2.33.1](https://github.com/mcereal/mesh-client/compare/v2.33.0...v2.33.1) (2026-09-07)

### Bug Fixes

* **ble:** retry a timed-out ServicesResolved poll instead of ending the link ([e65e566](https://github.com/mcereal/mesh-client/commit/e65e566631e1d3ad448a9ee5b9cbe3fb28c3e578))
* **core:** return from the event loop within its own timeout ([3ce22c0](https://github.com/mcereal/mesh-client/commit/3ce22c005649464cd58e2324c3ac17832e019030))

## [2.33.0](https://github.com/mcereal/mesh-client/compare/v2.32.1...v2.33.0) (2026-09-07)

### Features

* **ui:** a sparkline, and the sample ring behind it ([7bc8659](https://github.com/mcereal/mesh-client/commit/7bc8659f1c4b216695184e37f119c4ab751eb743))

### Bug Fixes

* **ui:** break a battery trend across external power, and keep the line legible under the cursor ([8f2b4d5](https://github.com/mcereal/mesh-client/commit/8f2b4d5fb54aeea3a044fd8f58e53915666fcd18)), closes [#105](https://github.com/mcereal/mesh-client/issues/105)

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
