"""The Arcade (FinalBurn Neo) sets' genres for the Library's filter (01.18, src/Ratings.h): each set's category -
catver.ini's ("Shooter / Flying Vertical", progettoSNAPS: THIRD_PARTY_NOTICES.md), as a table of set names, category,
year and maker gives it - put into LaunchBox's genres (src/Ratings.cpp's GENRES), the sets for adults apart.
Writes assets/xmb/ratings/arcade-genres.tsv: a line a genre (its LaunchBox name, or "Adult"), then its sets' names
apart with spaces - the smallest way (a set of two genres is in both lines). Only the name and the category are taken.
    python3 -I scripts/make-arcade-genres.py <table.tsv: name, category, ...> [out]"""
import os
import sys

# A category's main part (before " / ") -> its genres; a whole category first when it says more.
WHOLE = {
    "Platform / Fighter Scrolling": ["Beat 'em Up"],
    "Platform / Fighter": ["Beat 'em Up"],
    "Platform / Shooter Scrolling": ["Platform", "Shooter"],
    "Platform / Shooter": ["Platform", "Shooter"],
    "Platform / Maze": ["Platform"],
    "Maze / Shooter Small": ["Action", "Shooter"],
    "Maze / Shooter Large": ["Action", "Shooter"],
    "Maze / Fighter": ["Action", "Fighting"],
    "Maze / Driving": ["Action", "Racing"],
    "Multiplay / Mini-Games": ["Party"],
    "MultiGame / Mini-Games": ["Party"],
    "Misc. / Mini-Games": ["Party"],
    "Sports / Horse Racing": ["Sports", "Casino"],
    "Gambling / Horse Racing": ["Casino"],
    "Puzzle / Cards": ["Puzzle", "Board Game"],
    "Sports / Cards": ["Board Game"],
    "TTL / Ball & Paddle / Breakout": ["Action"],
    "Driving / Demolition Derby": ["Racing"],
}
MAIN = {
    "Shooter": ["Shooter"],
    "Fighter": ["Fighting"],
    "Platform": ["Platform"],
    "Climbing": ["Platform"],
    "Sports": ["Sports"],
    "Maze": ["Action"],
    "Ball & Paddle": ["Action"],
    "Whac-A-Mole": ["Action"],
    "Puzzle": ["Puzzle"],
    "Driving": ["Racing"],
    "Quiz": ["Quiz"],
    "Tabletop": ["Board Game"],
    "Board Game": ["Board Game"],
    "Gambling": ["Casino"],
    "Slot Machine": ["Casino"],
    "Medal Game": ["Casino"],
    "Redemption Game": ["Casino"],
    "Arcade": ["Pinball"],  # ("Arcade / Pinball")
    "Multiplay": ["Compilation"],
    "MultiGame": ["Compilation"],
    "Music Game": ["Music"],
    "Rhythm": ["Music"],
}
GENRES = ["Action", "Adventure", "Beat 'em Up", "Board Game", "Casino", "Compilation",
          "Construction and Management Simulation", "Education", "Fighting", "Flight Simulator", "Horror",
          "Life Simulation", "MMO", "Music", "Party", "Pinball", "Platform", "Puzzle", "Quiz", "Racing", "Role-Playing",
          "Sandbox", "Shooter", "Sports", "Stealth", "Strategy", "Vehicle Simulation", "Visual Novel"]
# The categories of sets for adults: the table's own word for them ("Adultos"), catver's mark.
ADULT = {"Adultos", "Adult", "Mature"}


def genres_of(category):
    category = category.strip()
    if category in ADULT or category.endswith("* Mature *"):
        return ["Adult"]
    if category in WHOLE:
        return WHOLE[category]
    return MAIN.get(category.split(" / ")[0].strip(), [])


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "assets", "xmb", "ratings", "arcade-genres.tsv")
    lines = {}
    sets = none = 0
    with open(sys.argv[1], encoding="utf-8") as f:
        for row in f:
            fields = row.rstrip("\r\n").split("\t")
            if len(fields) < 2 or fields[0] == "name" or not fields[0] or " " in fields[0]:
                continue
            sets += 1
            found = genres_of(fields[1])
            none += 0 if found else 1
            for genre in found:
                if genre != "Adult" and genre not in GENRES:
                    sys.exit("make-arcade-genres.py: not a LaunchBox genre: " + genre)
                lines.setdefault(genre, []).append(fields[0].lower())
    order = [g for g in GENRES if g in lines] + (["Adult"] if "Adult" in lines else [])
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# EmuSuNi: the Arcade (FinalBurn Neo) sets' genres for the Library's filter (src/Ratings.h), from their "
                "categories\n# (catver.ini, progettoSNAPS: THIRD_PARTY_NOTICES.md) by scripts/make-arcade-genres.py: "
                "a line a genre, then its sets.\n")
        for genre in order:
            f.write(genre + "\t" + " ".join(sorted(set(lines[genre]))) + "\n")
    print("%s: %d sets, %d of no genre; %s" % (out, sets, none,
                                              ", ".join("%s %d" % (g, len(set(lines[g]))) for g in order)))


if __name__ == "__main__":
    main()
