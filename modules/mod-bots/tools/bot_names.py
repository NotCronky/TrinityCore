#!/usr/bin/env python3
"""Builds sql/world/2026_10_05_03_mod_bots_names.sql: names for bot characters.

The names are written to look like a live WotLK realm: puns and jokes, gamer-style names, and plain
fantasy names. Each is tagged with a style and, where the name suggests one, a gender.

Every name is checked against the server's rules: 2 to 12 letters, letters only, no letter three times
in a row, and none of the client's reserved-name or profanity patterns (NamesReserved.dbc,
NamesProfanity.dbc, read from the extracted data). Names that fail are listed and left out.

    tools/bot_names.py [path/to/dbc]    (default: server/data/dbc in the repository)
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(HERE, "..", "sql", "world", "2026_10_05_03_mod_bots_names.sql")

# style: 0 plain fantasy, 1 funny, 2 gamer. gender: 0 male, 1 female, 2 either.
NORMAL, FUNNY, GAMER = 0, 1, 2
MALE, FEMALE, ANY = 0, 1, 2

FUNNY_NAMES = {
    ANY: """
        Tankyou Tankenstein Tankard Tankyoumuch Healsonwheel Holycow Holymoley Holysmokes Shamwow
        Shamanigans Shamtastic Dotsndots Dotmatrix Dotcom Ragequit Ragequitter Rageaholic Pewpew
        Pewpewlasers Frostbyte Manaburn Manabar Manaconda Stabbity Stabbyface Stabbymcstab Sneakypete
        Sneakysnake Ninjalooter Lootninja Gankalicious Corpsecamper Feardotcom Lockandload Lockstar
        Deathgrippy Gripntrip Unholycow Bloodbank Pallytank Bubblehearth Bubblebath Spamadin
        Arrowdynamic Bowjangles Shootsalot Petsmart Beastmode Boomchicken Treehugger Bearhug
        Bearlygrylls Catnap Catastrophe Druidiculous Smashmouth Hulksmash Bonkers Chargeit Ragemonster
        Whirlywind Sheepish Sheepthis Blinkandmiss Mageriffic Smitey Smitesalot Renewme Totemlyrad
        Totemsauce Chainheals Windfurious Earthshocker Bloodlusty Shivers Shankyou Shanksalot
        Kidneystone Afkmaster Bankalt Auctionalt Mailboxer Pugleader Tradechat Barrenschat Chucknorris
        Mankrikswife Leroyjenkins Ihavechicken Gnomepunter Gnomesayin Moomoo Moocow Udderchaos Notmilk
        Deadbeat Deadpan Bonejangles Brainz Trololo Voodoodoll Trollbridge Zugzug Orcward Workwork
        Jobsdone Gnomercy Shortstack Tinydeath Pocketrocket Beardyweirdy Pintsize Alecoholic Dwarfstar
        Elfonashelf Pointyears Sparklepants Glitterbomb Spacegoat Hoofhearted Humanbean Momsbasement
        Bedtimesoon Notmyalt Baconator Porkchop Meatloaf Cheddarbob Tacotuesday Burritos Pancakes
        Waffles Breadbox Hamsandwich Nachoman Meatball Picklejar Noodlearms Spaghettio Butterknife
        Toastmaster Biscuits Cupcakes Muffintop Pudding Gravyboat Saltandpepa Kebabs Donutdk Crumpet
        Bagelbites Sushiroll Frenchtoast Potatohead Soupoftheday Cornbread Applesauce Lettuce Tofuu
        Dpsmeter Recount Wipeagain Onemorepull Pullingmore Repaircost Whatsagrind Grindhouse Lagspike
        Disconnect Lootcouncil Dkpminus Rollneed Needonall Greedisgood Disenchanted Afkinbg Bgtourist
        Honorless Flagrunner Nodefender Cappedit Ganksquad Corpserun Spiritrez Rezplz Innkeeper
        Hearthstoned Flightpath Gryphonride Boatsahoy Zeppelinman Murloc Mrglmrgl Kobolds Youtakecandle
        Gnollsrule Defias Hogger Ragnaroslol Onyxiawhelp Deepbreath Morewhelps Fiftydkp Tooslow
        Mindcontrol Overpower Facepull Bodypull Aggrotastic Taunted Threatcap Hitcapped Crits Critsalot
        Crittycat Haste Spellpower Stamina Gearscore Epicfail Purplelewt Lootpinata Greenitems
        Vendortrash Grayitems Linenshirt Clothscraps Copperbar Mithrilbar Thorium Arcanite Netherweave
        Frostweave Herbalist Skinner Fishingpole Fishfinder Fishalot Bobber Cooking Sweetroll
        Crimsonbrew Murlocfins Raptorsteak Gooeyspider Lionsteak Pigsinablank Barbecue Darkmoon
        Fairground Tickets Carrotstick Ponyride Mountup Chopper Hogrider Ponytail Roflcopter Lolwut
        Omgwtf Brbpizza Brbcoffee Afkdinner Brbshower Mommysaysno Schoolnight Grounded Detention
        Homeworkdue Weekendwar Mondays Alarmclock Snoozebutton Notamorning Coffeebean Espresso
        Frappuccino Teatime Lemonade Applejuice Grapejuice Milkshake Smoothie Rootbeer Gingerale
        Fizzypop Bubbletea Waterbreak Drinkwater Hydrate Conjured Manastrudel Manacake Brainfreeze
        Goodgame Wellplayed Gogogo Inc Incoming Runaway Kitebot Kiteme Rootandboot Sheepandpeep
        Polyparrot Polypocket Pottytrained Healpot Manapot Flaskhunter Elixirman Bufftime Buffbot
        Fortitude Kingsbuff Mightymight Wisdom Thornsplz Motwplz Fortplz Pally Pallyhealz Healzplz
        Moarheals Moardots Moarpew Moardps Nomana Oom Outofmana Drinking Brbdrinking Waitforme Lagging
        Slowpoke Speedy Gonzalez Turbo Zoomzoom Vroom Sprinter Footrace Crawler Snail Turtletank
        Shelltank Sloth Lazybones Couchpotato Nappers Sleepyhead Yawnz Dreamer Moonwalker Shuffles
        Breakdance Chickendance Floss Dabbing Raisetheroof Partytime Funkymonkey Discoball Boogieman
        Grooves Jukebox Karaoke Lutebard Kazoo Tuba Bagpipes Cowbell Morecowbell Drumroll Tambourine
        Ukulele Banjo Fiddlesticks Harmonica Accordion
        Lootandscoot Spamheals Clickheals Keyturner Backpedal Backpedaler Keybinder Oneshot Twoshot
        Nerfme Buffme Nerfplz Bufftanks Lagmonster Pingpong Highping Packetloss Serverfirst Worldfirst
        Realmfirst Lastplace Wipeboss Bossmode Enragetimer Softenrage Raidwiper Wipeclub Autorun
        Autoattack Autoshot Wandspec Wandmaster Wandsalot Fishbreath Fishsticks Fishnchips Fishtaco
        Catfish Codfather Gillbert Clawsome Pawsome Meowzer Purrfect Catitude Hissyfit Barkbark Woofles
        Puppylove Hotdog Corndog Underdog Topdog Dogpile Snaccident Hangry Snackattack Snacktime
        Lunchbox Lunchlady Mealticket Breadwinner Breadstick Garlicknot Pizzaroll Pizzaparty Pizzaface
        Cheesepuff Cheeseball Cheesewheel Gouda Cheddar Sweetpea Peanut Jellybean Gummybear Sourpatch
        Marshmallow Smores Campfire Bonfire Firestarter Matchstick Kindling Firewood Sawdust Toothpick
        Paperclip Stapler Crayons Glitterglue Fingerpaint Doodlebug Scribbles Inkblot Sockpuppet
        Mismatch Laundry Dirtysocks Lintroller Dustbunny Broomstick Southshore Tarrenmill Crossroads
        Gurubashi Arenachest Bigglesworth Notabot Realperson Beepboop Bleepbloop Healzbot Totallyhuman
        Grumpycat Wrongbutton Missclick Keyboardcat Mousetrap Cheesetrap Bearbutt Moonkinfire
        Starfallen Treeform Pinecone Acorns Squirrel Hedgehog Badgerbadger Honeybadger Ferret
        Weaselface Otterpop Platypus Narwhal Penguin Walrus Pelican Toucan Flamingo Ostrich Llamadrama
        Alpaca Camelcase Giraffe Hippo Rhinoceros Antelope Gazelle Zebracakes Pandabear
        Koalabear Kangaroo Wombat Dingo Possum Racoon Skunkworks Chipmunk Hamsterwheel Gerbil Guineapig
        """,
    MALE: """
        Bubbleboy Magicmike Magicmikey Waterboy Averagejoe Justbob Garyoak Chadwick Brochacho
        Brosephus Dudebro Mrpibb Sirlootsalot Sirhealsalot Sirstabsalot Lordfluffy Kingtank Bigdaddy
        Daddycool Mrfreeze Mrbones Mrclean Mrbean Uncleted Grandpajoe Oldmangrim Grumpypants
        Beardedbob Kevin Kevinsdad Davethetank Stevethepally Bobthebuilder Normanbates Tinytim
        Bigjohn Littlejohn Robinhoodie Hoodlum Mrhoodie Dukenukem Captainobvious Sergeantpwn
        Doughboy Macroman Steveo Davey Kenny Timmy Jimbob Billybob Joebob Rickyb Bobbyb Frankthetank
        Dadjokes Dadbod Grandad Pops Unclebob Cousinvinny Bigtony Fattony Littleguy Bigguy Boomer Bubba
        Chuckles Buster Duke Rocco Tank Moose Bruiser Brutus Butch Spike Rex Max Bruno
        """,
    FEMALE: """
        Tankgirl Bubblegirl Watergirl Judgejudy Plainjane Dottie Healsnheels Lilmisshealz Missdots
        Ladyluck Queenbee Princesspeach Kittycat Catlady Grannypants Grannysmith Auntiem Mamabear
        Mamahealz Momsaysbed Karenshaman Sassypants Sparkleheart Glitterina Bubblegum Lollipop
        Cupcakequeen Sugarplum Honeybuns Cinnabon Pinkfluffy Fluffybunny Bunnyhop Ladystabsalot
        Missfortune Madamemim Ladygrim Lilredhood Rapunzel Goldilocks Snowwhite
        Lilprincess Missdemeanor Missmanaged Healingheart Momma Nana Granny Auntie Sissy Bestie
        Pumpkinpie Applepie Cherrypie Sweetiepie Cookiedough Buttercup Daisyduke Peaches Cinnamon
        Nutmeg Ginger Pepper Saltine Biscotti Macaron Tiramisu Cheesecake Strawberry Blueberry
        Raspberry Kiwi Mango Papaya Coconut Pineapple
        """,
}

GAMER_NAMES = {
    ANY: """
        Shadowstrike Deathblade Xshadowx Xxdarkxx Darkslayer Bloodreaper Soulreaper Nightshade
        Deathbringer Doomslayer Darkrevenge Pwnage Ownage Leetpally Imbaplayer Rektyou Noscope
        Shadowzz Healzz Frostboltz Holyshockz Lightbringr Healaholic Warlockk Huntaar Roguez Magez
        Pallyz Shamz Druidz Dkz Priestt Tankz Dpsz Stabz Frostz Firez Arcanez Dotz Pewz Shotz Bladez
        Shadowkill Bloodlust Deathwish Killshot Headshot Darknight Nightblade Nightfall Darkfall
        Shadowfall Deathfall Bloodfall Soulfall Doomfall Grimreaper Reaperx Xreaper Xkillerx Xslayerx
        Xbladex Xdeathx Xfrostx Xfirex Xstormx Xlightx Xholyx Xchaosx Xvenomx Xviperx Xghostx
        Ghostblade Ghostkill Venomstrike Viperstrike Chaosblade Chaoskill Stormblade Stormkill
        Frostblade Frostkill Fireblade Firekill Holyblade Lightblade Shadowblade Shadowfang Bloodfang
        Darkfang Deathfang Frostfang Stormfang Ironfang Steelfang Grimfang Doomfang Soulfang
        Shadowdeath Darkdeath Deathdeath Bloodshadow Darkshadow Shadowdark Darkness Blackout
        Blackheart Darkheart Coldheart Stoneheart Ironheart Deadheart Heartless Soulless Nameless
        Faceless Fearless Merciless Ruthless Relentless Bloodthirsty Insane Psycho Maniac Lunatic
        Madness Chaosmaster Pkmaster Gankmaster Duelmaster Arenamaster Glad Gladiator Duelist Rival
        Challenger Champion Legend Mythic Epic Rare Uncommon Leetness Prolevel Topdps Topheals
        Onlyheals Onlydps Dpsking Healqueen Tankking Pvpgod Pvegod Raidlead Guildmaster Officer
        Raider Hardcore Casual Noob Newbie Scrub Carry Smurf Twink Twinker Bankbot Farmbot Goldfarm
        Deadshot Snipez Quickscope Sniper Bulletproof Invincible Unstoppable Juggernaut Rampage
        Massacre Carnage Mayhem Havoc Wrecker Destroyer Annihilator Terminator Executioner Punisher
        Enforcer Avenger Vindicator Crusader Templar Holyknight Bladestorm Stormbringer Lightbane
        Shadowbane Frostbane Doombringer Soulstealer Lifestealer Bonecrusher Skullcrusher Skullsplitter
        Headhunter Manhunter Bloodhunter Nighthunter Darkhunter Shadowstalker Nightstalker Deathstalker
        Silentdeath Quietdeath Swiftdeath Suddendeath Instantdeath Painbringer Pain Agony Torment Misery
        Sorrow Despair Vengeance Revenge Retribution Wrath Fury Rage Hatred Malice Spite Venom Toxic
        """,
    MALE: """
        Darklord Deathlord Shadowlord Bloodlord Doomlord Stormlord Firelord Frostlord Warlord
        Darkking Deathking Shadowking Lichking Lichlord Overlord Kingslayer Godslayer Demonslayer
    """,
    FEMALE: """
        Darkangel Deathangel Bloodangel Shadowangel Angeldeath Angelface Darkqueen Deathqueen
        Shadowqueen Bloodqueen Icequeen Frostqueen Stormqueen Firequeen Queenofpain Ladydeath
        Ladydark Ladyshadow Ladyblood Ladyfrost Darkmistres Shadowmaid Battlemaiden Warmaiden
    """,
}

NORMAL_NAMES = {
    MALE: """
        Thorgrim Borin Durgan Kazrok Mogrin Gorrak Dravok Grimtusk Ragash Tormund Thandril Faelan
        Caelum Aldric Bramwell Cedric Darrow Edric Fenwick Garrick Halric Ivor Jareth Kendrick Lucan
        Merrick Orrin Percival Quentin Roderick Silas Tobias Ulric Varric Wendell Yorick Zander
        Brannic Durnan Gorvin Halgrim Khazdar Morgrim Norgrim Thorvik Ulfgar Bromdur Dain Gimrik
        Grundar Hrothgar Kildrum Murgan Rurik Tharnik Vondrak Aelric Caladin Eredin Faelor Galandir
        Ilyrin Kaelor Lorandil Maelis Nythandar Selvarin Thalindor Valandor Ysaryn Zephiron Belathor
        Daelithor Elarion Fyrith Ithilon Kaelthor Luthian Myrdin Rhaelan Taldorin Uldaris Grommok
        Krugash Lokthar Morgash  Orgrok Rakthar Skorn Thrukk Urgash Vorgash Zogrom Brakka
        Durotak  Hagrash Kragnor Mokgar Nargul Rogash Tagrak Uzgul Grimbul Zulkan  
        Vashji Zalgo Kazjin Jinrokh Raztal Zekhan Malakai   Banzal Daktar Hojin Kanjo
        Mazrak Tiraj Hakkar  Rahjin Taurag Bramhoof Oakhorn Thunderhoof Grimhorn Stormhoof
        Hawkhorn Stonehorn Ironhorn Elkhorn Runehoof Mudhoof Ashhorn    Hamuul Sunhoof
        Gorahn Kodorn Morlun Tarak Ulnar Varek Mordecai Lazarus Cornelius Ezekiel Malachai Thaddeus
        Barnabus Horatio Ignatius Leopold Montague Octavian Reginald Sebastian Theodore Valerian
        Winston Fizzwick Gizmont Tinkles Nimbleton Sprocket Cogsworth Fizzbang Wobblebolt Gearspin
        Ratchet Bolts Spindle Widget Gadgetz Tinkerton Nixel Pippin Fennick Boltwick Klaxon Cobbler
         Nobundo    Khadgar     Tarak   
          Darius Gideon Lorenzo Marcus Nathaniel Raymond Stefan Vincent Warren Alistair
        Benedict Desmond Everett Florian Gregor Harlan Jasper Lucius Magnus Nestor Oswald Rupert
        Alaric Bertram Caspian Dorian Emeric Fabian Godric Hadrian Isidore Jarvis Killian Leander
        Maximus Nicolai Phineas Quinlan Ronan Soren Tristan Ulysses Valen Wystan Xavier Yannick Zephyr
        Ansel Bastian Corwin Dashiell Emrys Finnick Gareth Hugo Idris Joren Lorcan Marek Niall Osric
        Pascal Rafe Stellan Thorne Urien Vance Wolfram Brom Dunstan Erland Fergus Gunnar Haldor Ivar
        Jorund Knut Leif Njal Olaf Ragnar Sven Torvald Ulf Vidar Bjorn Arne Eirik Grimbold Hamlin Grukk
        Durgash Kragg Garash Brogg Dregar Gulmok Harrok Karg Lugnar Margok Nogrul Orgath Rukar Shagg
        Torgash Ugrok Vargo Wrokk Zarg Bolgar Drukk Gorgul Krosh Mokrah Ragnok Snagg Zanji Rakjin Senjo
        Zuraj Kizan Mortimer Graves Barnaby Crowley Edgar Ichabod Ashworth Blackwood Crane Grimsby
        Hollis Mortain Ravencroft Sexton Vane Wildmane Plainsrunner Windtotem Fizzlewick Sparkwhistle
        Cogspin Tinkertop Bixby Pipwick Nubbins Fidget Gimble Sprocketz
    """,
    FEMALE: """
        Aelindra Velindra Seraphine Elowen Shalara Ysolde Brenna Kaelith Lyria Mirabel Nerissa
        Ophelia Rosalind Sabrielle Talia Ysabel Adalind Briar Celestine Delphine Evangeline Fiora
        Giselle Helena Isolde Juliet Katarina Liliana Marisol Natalia Odette Penelope Rowena Sienna
        Tamsin Vivienne Willow Xanthe Yvaine Zelda Aerith Alyndra Calenya Elenora Faeliana Ilyana
        Lirael Maelora Nalyndra Sylandra Thalassa Vaelora Ysera Aranel Celebrian Elwyn Ithilwen
        Lothiel Miriel Nimrodel Sorneth Taurwen Valandra Amberleaf Dawnwhisper Moonsong Starbreeze
        Silverleaf Sunblossom Windsong Brightleaf Mistwalker Nightsong Dawnstar Moonpetal Starwhisper
        Leafsong Stormsong Dewdrop Brynja Helga Ingrid Sigrun Astrid Freya Gudrun Hilda Ragna Solveig
        Thora Yrsa Dagny Elspeth Moira Bronwyn Glenna Kenna Morag Shona Tavia Gazra Mogra Narka Shazra
        Ugra Zalha Brakka Drogha Garona Gorza Kashra Morgana Rakka Thrala Urzha Zarkha Zulmara 
         Tiaja Zalani Kajira Malia Nalaya Rahja Sanjaa Vashra Zirala Yalanji Kahala Lunara
        Mahala Nayeli Sahale Tala Winona Aiyana Halona Kimimela Magena Nuttah Taini Ayita Yazhi
        Fizzy Gizzy Tinkerbelle Sprinkle Twinkle Nixie Pixie Widgetta Cogsy Bitsy Gearina Sparkla
        Wobbles Zippy Ishara Leyla Nerah Saria Tirra Valaria  Adara Emmara Kelara Lyndra Matrona
        Ambrosia Beatrix Cassandra Dorothea Eleanor Felicity Genevieve Henrietta Josephine Lucinda
        Margaret Octavia Philippa Rosamund Theodora Victoria Wilhelmina Annabel Clarissa Esmeralda
        Adelaide Beatrice Cordelia Daphne Elsbeth Fiona Gwendolyn Hazel Imogen Jessamine Keira Lorelei
        Maeve Nadia Oriana Primrose Rhiannon Saoirse Tabitha Ursula Verity Wren Yvette Zara Amara Calla
        Dahlia Elara Freesia Ginevra Iris Juniper Kalista Lavender Marigold Nova Opal Poppy Rosalie
        Saffron Tansy Violet Azalea Ember Indigo Luna Raven Sable Thistle Aurora Celeste Estella Elyria
        Kaelara Myriel Taelia Vanya Ziva Arura Kalaya Mehra Noori Sharaa Velari Zaria Grezka Murza
        Sharga Zugra Kozra Ragza Morticia Lenore Wednesday Annalise Bellamy Corinne Delilah Edwina
        Fernanda Georgina Harriet Isadora Jacinta Lucretia Mildred Nora Prudence Ramona Sybil Temperance
    """,
}


def load_patterns(dbc_dir):
    """The reserved and profanity patterns that apply to English clients."""
    patterns = []
    for name in ("NamesReserved.dbc", "NamesProfanity.dbc"):
        data = open(os.path.join(dbc_dir, name), "rb").read()
        _, count, fields, record_size, _ = struct.unpack_from("<4s4I", data, 0)
        strings = 20 + count * record_size
        for i in range(count):
            record = struct.unpack_from("<%dI" % fields, data, 20 + i * record_size)
            if record[2] not in (0, 0xFFFFFFFF):  # enUS, or every language
                continue
            text = data[strings + record[1]:].split(b"\0")[0].decode("utf-8", "replace")
            # The server compiles them as Boost Perl regexes, where \< and \> mark the start and end of a word.
            text = text.replace("\\<", "\\b").replace("\\>", "\\b")
            try:
                patterns.append((name, re.compile(text, re.IGNORECASE)))
            except re.error:
                pass
    return patterns


def problem(name, patterns):
    if not re.fullmatch(r"[A-Za-z]{2,12}", name):
        return "not 2 to 12 letters"
    if re.search(r"(.)\1\1", name.lower()):
        return "a letter three times in a row"
    for source, pattern in patterns:
        if pattern.search(name.lower()):
            return f"matches {pattern.pattern!r} in {source}"
    return None


def main():
    dbc_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "server", "data", "dbc")
    patterns = load_patterns(dbc_dir)

    rows = {}
    rejected = []
    for style, groups in ((FUNNY, FUNNY_NAMES), (GAMER, GAMER_NAMES), (NORMAL, NORMAL_NAMES)):
        for gender, text in groups.items():
            for name in text.split():
                name = name[0].upper() + name[1:].lower()
                if name.lower() in rows:
                    continue
                reason = problem(name, patterns)
                if reason:
                    rejected.append((name, reason))
                    continue
                rows[name.lower()] = (name, gender, style)

    for name, reason in rejected:
        print(f"left out {name}: {reason}")

    counts = {s: sum(1 for r in rows.values() if r[2] == s) for s in (NORMAL, FUNNY, GAMER)}
    print(f"{len(rows)} names: {counts[FUNNY]} funny, {counts[GAMER]} gamer, {counts[NORMAL]} plain; "
          f"{sum(1 for r in rows.values() if r[1] == MALE)} male, {sum(1 for r in rows.values() if r[1] == FEMALE)} female, "
          f"{sum(1 for r in rows.values() if r[1] == ANY)} either")

    with open(OUT, "w") as out:
        out.write("-- mod-bots: names for bot characters, generated by modules/mod-bots/tools/bot_names.py; edit the\n")
        out.write("-- lists there and run it again rather than editing this file. Every name passes the server's name\n")
        out.write("-- rules and the client's reserved-name and profanity lists.\n\n")
        out.write("DROP TABLE IF EXISTS `bot_names`;\n")
        out.write("CREATE TABLE `bot_names` (\n")
        out.write("  `name` VARCHAR(12) NOT NULL,\n")
        out.write("  `gender` TINYINT UNSIGNED NOT NULL COMMENT '0 male, 1 female, 2 either',\n")
        out.write("  `style` TINYINT UNSIGNED NOT NULL COMMENT '0 plain fantasy, 1 funny, 2 gamer',\n")
        out.write("  PRIMARY KEY (`name`)\n")
        out.write(") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;\n\n")
        out.write("INSERT INTO `bot_names` (`name`, `gender`, `style`) VALUES\n")
        values = [f"('{name}', {gender}, {style})" for name, gender, style in sorted(rows.values())]
        out.write(",\n".join(values) + ";\n")


if __name__ == "__main__":
    main()
