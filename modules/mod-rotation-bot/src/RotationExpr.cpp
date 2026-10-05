/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RotationExpr.h"
#include "RotationSpells.h"

#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"

#include <cctype>
#include <limits>
#include <memory>
#include <unordered_map>

namespace
{
    enum class TokenType : uint8
    {
        Number,
        Identifier,
        String,
        Operator,
        LeftParen,
        RightParen,
        Dot,
        Comma,
        End
    };

    struct Token
    {
        TokenType Type = TokenType::End;
        std::string Text;
        double Number = 0.0;
        std::size_t Column = 0; // 1-based.
    };

    struct ParseError
    {
        std::size_t Column;
        std::string Message;
    };

    [[noreturn]] void Fail(std::size_t column, std::string message)
    {
        throw ParseError{ column, std::move(message) };
    }

    bool IsIdentifierStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
    bool IsIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
    bool IsDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)); }

    std::vector<Token> Tokenize(std::string_view text)
    {
        std::vector<Token> tokens;
        std::size_t i = 0;
        while (i < text.size())
        {
            char c = text[i];
            std::size_t column = i + 1;

            if (std::isspace(static_cast<unsigned char>(c)))
            {
                ++i;
                continue;
            }

            if (IsDigit(c) || (c == '.' && i + 1 < text.size() && IsDigit(text[i + 1])))
            {
                std::size_t start = i;
                while (i < text.size() && (IsDigit(text[i]) || text[i] == '.'))
                    ++i;

                std::string number(text.substr(start, i - start));
                Optional<double> value = Trinity::StringTo<double>(number);
                if (!value)
                    Fail(column, Trinity::StringFormat("'{}' is not a number", number));

                tokens.push_back({ TokenType::Number, number, *value, column });
                continue;
            }

            if (IsIdentifierStart(c))
            {
                std::size_t start = i;
                while (i < text.size() && IsIdentifierChar(text[i]))
                    ++i;

                tokens.push_back({ TokenType::Identifier, std::string(text.substr(start, i - start)), 0.0, column });
                continue;
            }

            if (c == '"' || c == '\'')
            {
                std::size_t end = text.find(c, i + 1);
                if (end == std::string_view::npos)
                    Fail(column, "unterminated string");

                tokens.push_back({ TokenType::String, std::string(text.substr(i + 1, end - i - 1)), 0.0, column });
                i = end + 1;
                continue;
            }

            std::string_view rest = text.substr(i);
            bool matched = false;
            for (std::string_view op : { "<=", ">=", "==", "!=", "&&", "||" })
            {
                if (rest.substr(0, 2) == op)
                {
                    tokens.push_back({ TokenType::Operator, std::string(op), 0.0, column });
                    i += 2;
                    matched = true;
                    break;
                }
            }

            if (matched)
                continue;

            switch (c)
            {
                case '<': case '>': case '+': case '-': case '*': case '/': case '!':
                    tokens.push_back({ TokenType::Operator, std::string(1, c), 0.0, column });
                    break;
                case '(':
                    tokens.push_back({ TokenType::LeftParen, "(", 0.0, column });
                    break;
                case ')':
                    tokens.push_back({ TokenType::RightParen, ")", 0.0, column });
                    break;
                case '.':
                    tokens.push_back({ TokenType::Dot, ".", 0.0, column });
                    break;
                case ',':
                    tokens.push_back({ TokenType::Comma, ",", 0.0, column });
                    break;
                case '=':
                    Fail(column, "use == to compare");
                default:
                    Fail(column, Trinity::StringFormat("unexpected character '{}'", c));
            }

            ++i;
        }

        tokens.push_back({ TokenType::End, "end of condition", 0.0, text.size() + 1 });
        return tokens;
    }

    struct Argument
    {
        bool IsString = false;
        std::string Text;
        double Number = 0.0;
        std::size_t Column = 0;
    };

    // One step of a dotted path such as target.my_aura("Rend").remains.
    struct Segment
    {
        std::string Name;
        bool Called = false;
        std::vector<Argument> Arguments;
        std::size_t Column = 0;
    };

    using UnitSelector = UnitSnapshot const* (*)(AplContext const&);
    using UnitField = double (*)(UnitSnapshot const&);
    using StateField = double (*)(AplContext const&);

    UnitSnapshot const* SelectPlayer(AplContext const& c) { return &c.State.Self; }
    UnitSnapshot const* SelectTarget(AplContext const& c) { return c.State.Target ? &*c.State.Target : nullptr; }
    UnitSnapshot const* SelectUnit(AplContext const& c) { return c.Unit; }
    UnitSnapshot const* SelectPet(AplContext const& c) { return c.State.Pet ? &*c.State.Pet : nullptr; }

    // The live unit behind a snapshot, for what only the server knows: diminishing returns and
    // another player's cooldowns. Same map and same tick as the snapshot.
    Unit* LiveUnit(AplContext const& c, UnitSnapshot const& snapshot)
    {
        if (snapshot.Guid == c.Me->GetGUID())
            return c.Me;

        return ObjectAccessor::GetUnit(*c.Me, snapshot.Guid);
    }

    std::unordered_map<std::string_view, uint8> const DISPEL_TYPES =
    {
        { "magic",   DISPEL_MAGIC },
        { "curse",   DISPEL_CURSE },
        { "disease", DISPEL_DISEASE },
        { "poison",  DISPEL_POISON }
    };

    double Bool(bool value) { return value ? 1.0 : 0.0; }

    std::unordered_map<std::string_view, UnitField> const UNIT_FIELDS =
    {
        { "health_pct",         [](UnitSnapshot const& u) { return double(u.HealthPct); } },
        { "level",              [](UnitSnapshot const& u) { return double(u.Level); } },
        { "is_player",          [](UnitSnapshot const& u) { return Bool(u.IsPlayer); } },
        { "is_boss",            [](UnitSnapshot const& u) { return Bool(u.IsBoss); } },
        { "stunned",            [](UnitSnapshot const& u) { return Bool(u.Stunned); } },
        { "rooted",             [](UnitSnapshot const& u) { return Bool(u.Rooted); } },
        { "feared",             [](UnitSnapshot const& u) { return Bool(u.Feared); } },
        { "confused",           [](UnitSnapshot const& u) { return Bool(u.Confused); } },
        { "silenced",           [](UnitSnapshot const& u) { return Bool(u.Silenced); } },
        { "disarmed",           [](UnitSnapshot const& u) { return Bool(u.Disarmed); } },
        { "casting",            [](UnitSnapshot const& u) { return Bool(u.Cast.has_value()); } },
        { "cast_remains",       [](UnitSnapshot const& u) { return u.Cast ? u.Cast->RemainingMs / 1000.0 : 0.0; } },
        { "cast_interruptible", [](UnitSnapshot const& u) { return Bool(u.Cast && u.Cast->Interruptible); } },
        { "cast_channeled",     [](UnitSnapshot const& u) { return Bool(u.Cast && u.Cast->Channeled); } },
        { "cast_heal",          [](UnitSnapshot const& u) { return Bool(u.Cast && u.Cast->IsHeal); } },
        { "cast_cc",            [](UnitSnapshot const& u) { return Bool(u.Cast && u.Cast->IsCrowdControl); } },
        { "hard_cc",            [](UnitSnapshot const& u) { return Bool(u.Stunned || u.Feared || u.Confused); } },
        { "cc_remains",         [](UnitSnapshot const& u)
            {
                int32 ms = u.CrowdControlRemainingMs();
                return ms == std::numeric_limits<int32>::max() ? std::numeric_limits<double>::infinity() : ms / 1000.0;
            } },
        { "exists",             [](UnitSnapshot const&) { return 1.0; } },
        { "hostile",            [](UnitSnapshot const& u) { return Bool(u.Hostile); } },
        { "distance",           [](UnitSnapshot const& u) { return double(u.Distance); } },
        { "in_melee",           [](UnitSnapshot const& u) { return Bool(u.InMelee); } },
        { "los",                [](UnitSnapshot const& u) { return Bool(u.InLos); } },
        { "facing",             [](UnitSnapshot const& u) { return Bool(u.Facing); } },
        { "behind",             [](UnitSnapshot const& u) { return Bool(u.Behind); } },
        { "threat_pct",         [](UnitSnapshot const& u) { return double(u.ThreatPct); } },
        { "targeting_me",       [](UnitSnapshot const& u) { return Bool(u.TargetingMe); } },
        { "targeting_party",    [](UnitSnapshot const& u) { return Bool(u.TargetingParty); } },
        { "attackers",          [](UnitSnapshot const& u) { return double(u.Attackers); } },
        { "most_attacked",      [](UnitSnapshot const& u) { return Bool(u.MostAttacked); } }
    };

    std::unordered_map<std::string_view, StateField> const PLAYER_FIELDS =
    {
        { "power",        [](AplContext const& c) { return double(c.State.Power); } },
        { "max_power",    [](AplContext const& c) { return double(c.State.MaxPower); } },
        { "power_pct",    [](AplContext const& c) { return double(c.State.PowerPct()); } },
        { "combo_points", [](AplContext const& c) { return double(c.State.ComboPoints); } },
        { "runes_blood",  [](AplContext const& c) { return double(c.State.ReadyRunes[0]); } },
        { "runes_unholy", [](AplContext const& c) { return double(c.State.ReadyRunes[1]); } },
        { "runes_frost",  [](AplContext const& c) { return double(c.State.ReadyRunes[2]); } },
        { "runes_death",  [](AplContext const& c) { return double(c.State.ReadyRunes[3]); } },
        { "moving",       [](AplContext const& c) { return Bool(c.State.Moving); } },
        { "gcd",          [](AplContext const& c) { return c.State.GcdRemainingMs / 1000.0; } },
        { "in_combat",    [](AplContext const& c) { return Bool(c.Me->IsInCombat()); } },
        { "auto_repeat",  [](AplContext const& c) { return Bool(c.State.AutoRepeat); } },
        { "mainhand_enchanted", [](AplContext const& c) { return Bool(c.State.MainHandEnchanted); } },
        { "offhand_enchanted",  [](AplContext const& c) { return Bool(c.State.OffHandEnchanted); } },
        { "totem_fire",   [](AplContext const& c) { return Bool(c.State.Totems[0]); } },
        { "totem_earth",  [](AplContext const& c) { return Bool(c.State.Totems[1]); } },
        { "totem_water",  [](AplContext const& c) { return Bool(c.State.Totems[2]); } },
        { "totem_air",    [](AplContext const& c) { return Bool(c.State.Totems[3]); } }
    };

    enum class AuraProperty : uint8
    {
        Up,
        Remains,
        Stacks,
        Charges
    };

    double ReadAura(AuraSnapshot const* aura, AuraProperty property)
    {
        if (!aura)
            return 0.0;

        switch (property)
        {
            case AuraProperty::Up:
                return 1.0;
            case AuraProperty::Remains:
                return aura->RemainingMs < 0 ? std::numeric_limits<double>::infinity() : aura->RemainingMs / 1000.0;
            case AuraProperty::Stacks:
                return aura->Stacks;
            case AuraProperty::Charges:
                return aura->Charges;
        }

        return 0.0;
    }

    using SpellList = std::vector<SpellRef>;

    // The first aura on the unit matching any of the spells.
    AuraSnapshot const* FindAura(UnitSnapshot const& unit, SpellList const& spells, bool mineOnly)
    {
        for (AuraSnapshot const& aura : unit.Auras)
        {
            if (!aura.Mine && mineOnly)
                continue;

            for (SpellRef const& spell : spells)
                if (spell.Matches(aura.SpellId))
                    return &aura;
        }

        return nullptr;
    }

    class Parser
    {
    public:
        explicit Parser(std::vector<Token> tokens) : _tokens(std::move(tokens)) {}

        AplExpr ParseAll()
        {
            AplExpr expr = ParseOr();
            if (Peek().Type != TokenType::End)
                Fail(Peek().Column, Trinity::StringFormat("unexpected '{}'", Peek().Text));

            return expr;
        }

    private:
        Token const& Peek() const { return _tokens[_pos]; }

        Token const& Advance()
        {
            Token const& token = _tokens[_pos];
            if (token.Type != TokenType::End)
                ++_pos;

            return token;
        }

        bool AcceptOperator(std::string_view op)
        {
            if (Peek().Type != TokenType::Operator || Peek().Text != op)
                return false;

            Advance();
            return true;
        }

        bool AcceptKeyword(std::string_view word)
        {
            if (Peek().Type != TokenType::Identifier || Peek().Text != word)
                return false;

            Advance();
            return true;
        }

        Token const& Expect(TokenType type, char const* what)
        {
            if (Peek().Type != type)
                Fail(Peek().Column, Trinity::StringFormat("expected {}, found '{}'", what, Peek().Text));

            return Advance();
        }

        AplExpr ParseOr()
        {
            AplExpr left = ParseAnd();
            while (AcceptKeyword("or") || AcceptOperator("||"))
            {
                AplExpr right = ParseAnd();
                left = [left, right](AplContext const& c) { return Bool(left(c) != 0.0 || right(c) != 0.0); };
            }

            return left;
        }

        AplExpr ParseAnd()
        {
            AplExpr left = ParseNot();
            while (AcceptKeyword("and") || AcceptOperator("&&"))
            {
                AplExpr right = ParseNot();
                left = [left, right](AplContext const& c) { return Bool(left(c) != 0.0 && right(c) != 0.0); };
            }

            return left;
        }

        AplExpr ParseNot()
        {
            if (AcceptKeyword("not") || AcceptOperator("!"))
            {
                AplExpr inner = ParseNot();
                return [inner](AplContext const& c) { return Bool(inner(c) == 0.0); };
            }

            return ParseComparison();
        }

        AplExpr ParseComparison()
        {
            AplExpr left = ParseSum();

            if (AcceptOperator("<"))
                return Compare(left, ParseSum(), [](double a, double b) { return a < b; });
            if (AcceptOperator("<="))
                return Compare(left, ParseSum(), [](double a, double b) { return a <= b; });
            if (AcceptOperator(">"))
                return Compare(left, ParseSum(), [](double a, double b) { return a > b; });
            if (AcceptOperator(">="))
                return Compare(left, ParseSum(), [](double a, double b) { return a >= b; });
            if (AcceptOperator("=="))
                return Compare(left, ParseSum(), [](double a, double b) { return a == b; });
            if (AcceptOperator("!="))
                return Compare(left, ParseSum(), [](double a, double b) { return a != b; });

            return left;
        }

        static AplExpr Compare(AplExpr left, AplExpr right, bool (*op)(double, double))
        {
            return [left, right, op](AplContext const& c) { return Bool(op(left(c), right(c))); };
        }

        AplExpr ParseSum()
        {
            AplExpr left = ParseProduct();
            while (true)
            {
                if (AcceptOperator("+"))
                {
                    AplExpr right = ParseProduct();
                    left = [left, right](AplContext const& c) { return left(c) + right(c); };
                }
                else if (AcceptOperator("-"))
                {
                    AplExpr right = ParseProduct();
                    left = [left, right](AplContext const& c) { return left(c) - right(c); };
                }
                else
                    return left;
            }
        }

        AplExpr ParseProduct()
        {
            AplExpr left = ParseUnary();
            while (true)
            {
                if (AcceptOperator("*"))
                {
                    AplExpr right = ParseUnary();
                    left = [left, right](AplContext const& c) { return left(c) * right(c); };
                }
                else if (AcceptOperator("/"))
                {
                    AplExpr right = ParseUnary();
                    left = [left, right](AplContext const& c)
                    {
                        double divisor = right(c);
                        return divisor != 0.0 ? left(c) / divisor : 0.0;
                    };
                }
                else
                    return left;
            }
        }

        AplExpr ParseUnary()
        {
            if (AcceptOperator("-"))
            {
                AplExpr inner = ParseUnary();
                return [inner](AplContext const& c) { return -inner(c); };
            }

            return ParsePrimary();
        }

        AplExpr ParsePrimary()
        {
            Token const& token = Peek();
            switch (token.Type)
            {
                case TokenType::Number:
                {
                    double value = Advance().Number;
                    return [value](AplContext const&) { return value; };
                }
                case TokenType::LeftParen:
                {
                    Advance();
                    AplExpr inner = ParseOr();
                    Expect(TokenType::RightParen, "')'");
                    return inner;
                }
                case TokenType::Identifier:
                {
                    if (AcceptKeyword("true"))
                        return [](AplContext const&) { return 1.0; };

                    if (AcceptKeyword("false"))
                        return [](AplContext const&) { return 0.0; };

                    if (token.Text == "and" || token.Text == "or" || token.Text == "not")
                        Fail(token.Column, Trinity::StringFormat("expected a value before '{}'", token.Text));

                    return Resolve(ParsePath());
                }
                default:
                    Fail(token.Column, Trinity::StringFormat("expected a value, found '{}'", token.Text));
            }
        }

        std::vector<Segment> ParsePath()
        {
            std::vector<Segment> path;
            do
            {
                Token const& name = Expect(TokenType::Identifier, "a name");
                Segment segment;
                segment.Name = name.Text;
                segment.Column = name.Column;

                if (Peek().Type == TokenType::LeftParen)
                {
                    Advance();
                    segment.Called = true;
                    if (Peek().Type != TokenType::RightParen)
                    {
                        do
                        {
                            Token const& arg = Advance();
                            if (arg.Type != TokenType::Number && arg.Type != TokenType::String)
                                Fail(arg.Column, "arguments must be numbers or quoted spell names");

                            bool isString = arg.Type == TokenType::String;
                            segment.Arguments.push_back({ isString, arg.Text, arg.Number, arg.Column });
                        } while (Peek().Type == TokenType::Comma && (Advance(), true));
                    }

                    Expect(TokenType::RightParen, "')'");
                }

                path.push_back(std::move(segment));
            } while (Peek().Type == TokenType::Dot && (Advance(), true));

            return path;
        }

        static void ExpectArguments(Segment const& segment, std::size_t count)
        {
            if (count == 0 && segment.Called)
                Fail(segment.Column, Trinity::StringFormat("'{}' takes no arguments", segment.Name));

            if (count > 0 && (!segment.Called || segment.Arguments.size() != count))
                Fail(segment.Column, Trinity::StringFormat("'{}' takes {} argument{}", segment.Name, count,
                    count == 1 ? "" : "s"));
        }

        static void ExpectEnd(std::vector<Segment> const& path, std::size_t length)
        {
            if (path.size() > length)
                Fail(path[length].Column, Trinity::StringFormat("'{}' has no field '{}'", path[length - 1].Name,
                    path[length].Name));
        }

        static float NumberArgument(Segment const& segment)
        {
            ExpectArguments(segment, 1);
            Argument const& arg = segment.Arguments[0];
            if (arg.IsString)
                Fail(arg.Column, Trinity::StringFormat("'{}' takes a number", segment.Name));

            return float(arg.Number);
        }

        static SpellRef ParseSpellArgument(Argument const& arg)
        {
            Optional<SpellRef> spell = arg.IsString ? SpellRef::Parse(arg.Text) : SpellRef::FromId(uint32(arg.Number));
            if (!spell)
                Fail(arg.Column, Trinity::StringFormat("no spell is named '{}'", arg.Text));

            return std::move(*spell);
        }

        static std::shared_ptr<SpellRef const> SpellArgument(Segment const& segment)
        {
            ExpectArguments(segment, 1);
            return std::make_shared<SpellRef const>(ParseSpellArgument(segment.Arguments[0]));
        }

        // One or more spells, any of which counts: aura("Seal of Command", "Seal of Vengeance").
        static std::shared_ptr<SpellList const> SpellArguments(Segment const& segment)
        {
            if (!segment.Called || segment.Arguments.empty())
                Fail(segment.Column, Trinity::StringFormat("'{}' takes one or more spell names", segment.Name));

            SpellList spells;
            for (Argument const& arg : segment.Arguments)
                spells.push_back(ParseSpellArgument(arg));

            return std::make_shared<SpellList const>(std::move(spells));
        }

        static AplExpr Resolve(std::vector<Segment> const& path)
        {
            Segment const& root = path[0];

            if (root.Name == "player" || root.Name == "target" || root.Name == "unit" || root.Name == "pet")
            {
                ExpectArguments(root, 0);
                if (path.size() < 2)
                    Fail(root.Column, Trinity::StringFormat("'{}' needs a field, such as {}.health_pct", root.Name,
                        root.Name));

                UnitSelector unit = root.Name == "player" ? SelectPlayer : root.Name == "target" ? SelectTarget :
                    root.Name == "pet" ? SelectPet : SelectUnit;
                return ResolveUnit(root.Name, unit, path);
            }

            if (root.Name == "pvp" || root.Name == "pve" || root.Name == "aoe" || root.Name == "burst")
            {
                ExpectArguments(root, 0);
                ExpectEnd(path, 1);
                if (root.Name == "pvp")
                    return [](AplContext const& c) { return Bool(c.Settings.Pvp); };

                if (root.Name == "pve")
                    return [](AplContext const& c) { return Bool(!c.Settings.Pvp); };

                if (root.Name == "aoe")
                    return [](AplContext const& c) { return Bool(c.Settings.Aoe); };

                return [](AplContext const& c) { return Bool(c.Settings.Burst); };
            }

            if (root.Name == "enemies")
            {
                float radius = NumberArgument(root);
                ExpectEnd(path, 1);
                return [radius](AplContext const& c)
                {
                    return double(c.State.EnemiesNear(c.Me->GetPosition(), radius));
                };
            }

            if (root.Name == "party_my_aura")
            {
                std::shared_ptr<SpellList const> spells = SpellArguments(root);
                ExpectEnd(path, 1);
                return [spells](AplContext const& c)
                {
                    uint32 count = 0;
                    for (UnitSnapshot const& member : c.State.Party)
                        count += FindAura(member, *spells, true) != nullptr;

                    return double(count);
                };
            }

            if (root.Name == "party_below")
            {
                float pct = NumberArgument(root);
                ExpectEnd(path, 1);
                return [pct](AplContext const& c)
                {
                    uint32 count = 0;
                    for (UnitSnapshot const& member : c.State.Party)
                        count += member.HealthPct < pct;

                    return double(count);
                };
            }

            if (root.Name == "cooldown")
            {
                std::shared_ptr<SpellRef const> spell = SpellArgument(root);
                ExpectEnd(path, 1);
                return [spell](AplContext const& c)
                {
                    uint32 rank = spell->KnownRank(c.Me);
                    return rank ? c.Me->GetSpellHistory()->GetRemainingCooldown(sSpellMgr->AssertSpellInfo(rank)) / 1000.0 : 0.0;
                };
            }

            if (root.Name == "known")
            {
                std::shared_ptr<SpellRef const> spell = SpellArgument(root);
                ExpectEnd(path, 1);
                return [spell](AplContext const& c) { return Bool(spell->KnownRank(c.Me) != 0); };
            }

            Fail(root.Column, Trinity::StringFormat("unknown name '{}'; expected player, target, unit, pet, enemies, "
                "party_below, party_my_aura, cooldown, known, pvp, pve, aoe or burst", root.Name));
        }

        static AplExpr ResolveUnit(std::string const& rootName, UnitSelector unit, std::vector<Segment> const& path)
        {
            bool isPlayer = rootName == "player";
            Segment const& field = path[1];

            if (field.Name == "aura" || field.Name == "my_aura")
            {
                std::shared_ptr<SpellList const> spells = SpellArguments(field);
                bool mineOnly = field.Name == "my_aura";

                AuraProperty property = AuraProperty::Up;
                if (path.size() > 2)
                {
                    Segment const& name = path[2];
                    ExpectArguments(name, 0);
                    if (name.Name == "up")
                        property = AuraProperty::Up;
                    else if (name.Name == "remains")
                        property = AuraProperty::Remains;
                    else if (name.Name == "stacks")
                        property = AuraProperty::Stacks;
                    else if (name.Name == "charges")
                        property = AuraProperty::Charges;
                    else
                        Fail(name.Column, Trinity::StringFormat("an aura has up, remains, stacks and charges, not '{}'",
                            name.Name));
                }

                ExpectEnd(path, 3);
                return [unit, spells, mineOnly, property](AplContext const& c)
                {
                    UnitSnapshot const* snapshot = unit(c);
                    return snapshot ? ReadAura(FindAura(*snapshot, *spells, mineOnly), property) : 0.0;
                };
            }

            if (field.Name == "casting" && field.Called)
            {
                std::shared_ptr<SpellRef const> spell = SpellArgument(field);
                ExpectEnd(path, 2);
                return [unit, spell](AplContext const& c)
                {
                    UnitSnapshot const* snapshot = unit(c);
                    return Bool(snapshot && snapshot->Cast && spell->Matches(snapshot->Cast->SpellId));
                };
            }

            if (field.Name == "enemies")
            {
                float radius = NumberArgument(field);
                ExpectEnd(path, 2);
                if (isPlayer)
                    return [radius](AplContext const& c)
                    {
                        return double(c.State.EnemiesNear(c.Me->GetPosition(), radius));
                    };

                return [unit, radius](AplContext const& c)
                {
                    UnitSnapshot const* snapshot = unit(c);
                    return snapshot ? double(c.State.EnemiesNear(snapshot->Pos, radius)) : 0.0;
                };
            }

            if (field.Name == "dr")
            {
                std::shared_ptr<SpellRef const> spell = SpellArgument(field);
                ExpectEnd(path, 2);
                return [unit, spell](AplContext const& c)
                {
                    UnitSnapshot const* snapshot = unit(c);
                    if (!snapshot)
                        return 0.0;

                    return double(GetDiminishingLevel(LiveUnit(c, *snapshot), spell->Representative()));
                };
            }

            if (field.Name == "cooldown")
            {
                std::shared_ptr<SpellRef const> spell = SpellArgument(field);
                ExpectEnd(path, 2);
                return [unit, spell](AplContext const& c)
                {
                    UnitSnapshot const* snapshot = unit(c);
                    Unit* live = snapshot ? LiveUnit(c, *snapshot) : nullptr;
                    Player* other = live ? live->ToPlayer() : nullptr;
                    if (!other)
                        return 0.0;

                    uint32 rank = spell->KnownRank(other);
                    return rank ? other->GetSpellHistory()->GetRemainingCooldown(sSpellMgr->AssertSpellInfo(rank)) / 1000.0 : 0.0;
                };
            }

            if (field.Name == "dispellable")
            {
                ExpectArguments(field, 1);
                Argument const& arg = field.Arguments[0];
                auto type = DISPEL_TYPES.find(arg.Text);
                if (!arg.IsString || type == DISPEL_TYPES.end())
                    Fail(arg.Column, "dispellable takes \"magic\", \"curse\", \"disease\" or \"poison\"");

                uint8 dispelType = type->second;
                ExpectEnd(path, 2);
                return [unit, dispelType](AplContext const& c)
                {
                    UnitSnapshot const* snapshot = unit(c);
                    return snapshot ? double(snapshot->Dispellable(dispelType)) : 0.0;
                };
            }

            ExpectArguments(field, 0);
            ExpectEnd(path, 2);

            if (auto itr = UNIT_FIELDS.find(field.Name); itr != UNIT_FIELDS.end())
            {
                UnitField read = itr->second;
                return [unit, read](AplContext const& c)
                {
                    UnitSnapshot const* snapshot = unit(c);
                    return snapshot ? read(*snapshot) : 0.0;
                };
            }

            if (isPlayer)
            {
                if (auto itr = PLAYER_FIELDS.find(field.Name); itr != PLAYER_FIELDS.end())
                {
                    StateField read = itr->second;
                    return [read](AplContext const& c) { return read(c); };
                }
            }

            Fail(field.Column, Trinity::StringFormat("{} has no field '{}'", rootName, field.Name));
        }

        std::vector<Token> _tokens;
        std::size_t _pos = 0;
    };
}

Optional<AplExpr> RotationExpr::Parse(std::string_view text, std::string& error)
{
    try
    {
        return Parser(Tokenize(text)).ParseAll();
    }
    catch (ParseError const& e)
    {
        error = Trinity::StringFormat("column {}: {}", e.Column, e.Message);
        return {};
    }
}
