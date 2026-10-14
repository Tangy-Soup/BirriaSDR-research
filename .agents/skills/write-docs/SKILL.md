---
name: write-docs
description: BirriaSDR documentation standards for writing Markdown (README.md and docs/*.md). Use whenever writing or editing any documentation in this repository, such as a new section, a register reference, an API description, test notes or a README update.
---

# Writing BirriaSDR documentation

Our documentation describes experimental hardware. Readers use it to learn what
is implemented, what was measured and what is still unknown. Every page should
be accurate first, then clear, then pleasant to read. Write it to these
standards from the first draft.

## Choose the right file

Each file serves one reader. Put new content where that reader will look, and
pitch the language to them.

| File | Reader | What belongs there |
| --- | --- | --- |
| `README.md` | Someone deciding whether to use the library, or using it for the first time | What the project does, specifications, build and run steps, API overview, setup pitfalls, license and acknowledgements. Plain language, with links to the docs for internals |
| `docs/hardware.md` | A user who wants to understand the receive path and its guarantees | Hardware and software roles, ownership, acquisition and recovery behavior, without register detail |
| `docs/mt7921.md` | Someone debugging or extending the MT7921 backend | Registers, bit fields, sequences, memory layout, mailbox protocol and data path |
| `docs/architecture.md` | A contributor adding code, a chipset or tests | Source layout, extension steps, error and threading rules, test scope |

A new chipset backend gets its own reference page, modeled on
`docs/mt7921.md`, plus a row in the architecture file table.

The README stays the least technical page, but it still gives real information.
Describe the effect a user cares about and leave the mechanism to the docs. For
example, the README says the library "checks every packet for gaps, repeats,
stale data and corruption", while `docs/mt7921.md` names the sequence, nonce
and framing checks.

## Write sentences

Prose and any table cell that forms a sentence follow these rules.

- **Start every sentence with a word.** A sentence or list item never begins
  with an acronym, a number, inline code, a link or a URL. Lead with an article
  or a noun that names the thing.

  | Write | Not |
  | --- | --- |
  | The MMIO register at `0x70010200` holds the digital ID. | MMIO `0x70010200` holds the digital ID. |
  | The only supported rate is 40 MSps. | 40 MSps is the only supported rate. |
  | The `read_sync` function captures a fixed number of samples. | `read_sync` captures a fixed number of samples. |
  | The MT7925 backend is not implemented. | MT7925 is not implemented. |
  | The upstream [mt76](https://github.com/openwrt/mt76) Linux driver... | [mt76](https://github.com/openwrt/mt76) is the upstream driver... |

  Part numbers (MT7921), protocol words (USB, DMA, RF, GO, STOP) and I/Q all
  count as acronyms.
- **Use only commas, colons, parentheses and periods to join ideas.** Do not
  write em dashes (—), en dashes (–) or semicolons. When two clauses need
  joining, use "and", "but", "so" or "because", or write two sentences.
- **Write ranges with words**, as in "2156 to 2610 MHz" or "LNA 0 to 3".
- **Skip the Oxford comma**, as in "clocks, tuning and gain".
- **Keep sentences short and complete.** One idea per sentence reads better
  than a chain of clauses. Prefer the active voice when it names a clear actor,
  such as "The host queues reads before releasing GO."
- **Leave out filler** such as "simply", "just", "basically", "note that" or
  "it should be noted".
- **Address the reader as "you"** in the README and in instructions. Reference
  pages describe the hardware and library in the third person.
- **Use "our"** only for things this project wrote, such as "our original
  firmware".

## Describe evidence precisely

Readers make decisions from these words, so use them consistently.

| Word | Meaning |
| --- | --- |
| measured | Observed on real hardware, with the board stated |
| unmeasured | Not yet observed, even if the software supports it |
| validated, qualified | Tested on hardware and enabled in the library |
| unqualified | Recognized or possible, but not enabled or not tested |
| uncalibrated | Works, but values do not map to physical units |
| unknown, not decoded, unresolved | The meaning or effect has not been established |
| nominal | The designed value, not a measured one |

Follow these principles when you describe behavior:

- A register write, a readback or a capability bit is not proof of a physical
  effect. Say what the evidence shows and stop there, as in "Completion is not
  a qualified lock indication."
- State limits next to the feature. Most tables have a Limits or Scope column
  for this.
- Document unsupported controls, untested platforms and open questions. They
  help readers as much as features do.
- Describe failures as errors that the caller sees. When you describe a retry,
  fallback or recovery, also say when it does not apply.
- Name the tested board whenever you quote a measurement, as in "on the tested
  board" or "on the tested Raspberry Pi".

## Format values and identifiers

- **Units** take a space and standard symbols: 40 MHz, 40 MSps, 87 ms,
  20 microseconds, 64 KiB, 161 MB/s. Say "40 MSps complex" when the sample type
  matters.
- **Numbers** with four or more digits take commas when they are counts, sizes
  or values in Hz, such as 1,120 bytes, 65,536 samples or 2,437,000,000 Hz.
  Frequencies in MHz and sample component values stay without commas, such as
  2437 MHz or −8192 to 8191. Spell out counts from one to ten when they
  are not measurements or values, such as "four descriptors", "three restarts"
  or "ten CTest cases". Keep digits for values, indices and units, such as
  "chain 0", "bit 12" or "code 30".
- **Negative values** use the minus sign (−8192), not a hyphen.
- **Hexadecimal** is lowercase with a `0x` prefix inside inline code, such as
  `0x830ad448`.
- **Bit fields** use `[high:low]` inside inline code, such as `[19:18]`, and a
  single bit is written as "bit 23" or `[23]`.
- **Register addresses** name the access path first and the address second, as
  in MMIO `0x830ad448`, RF port 1 `0x768` or TOP `0x02c`.
- **Inline code** marks identifiers, file paths, commands, flags, register
  addresses, bit fields and literal values. Prose words such as "chain 0" stay
  plain.
- **Functions** appear with parentheses on reference pages, as in
  `birriasdr_set_center_freq()`, and are introduced as "the ... function" or
  "the ... call".

## Format Markdown

**Headings**

- Use one H1 per page, then H2 for sections and H3 for subsections. Do not go
  deeper than H3.
- Write headings in sentence case. Reference pages name sections by activity,
  as in "Preparing the device" or "Capturing with ICAP". The README uses short
  task headings such as "Build and try" or "Install and link".
- Treat a heading as a stable anchor. Other pages link to
  `README.md#build-and-try`, `mt7921.md#filtering`,
  `mt7921.md#changing-frequency-during-capture`,
  `mt7921.md#changing-gain-during-capture`,
  `mt7921.md#automatic-startup-and-stream-recovery` and
  `architecture.md#tests-and-firmware`. Before renaming any heading, find its
  links with `grep -rn '#anchor-name' README.md docs/` and update them.

**Paragraphs and lists**

- Wrap prose near 80 columns. Table rows and list items may run long.
- The note block in `cyberether/birriasdr.yml` is the exception. CyberEther
  renders every newline as a line break, so write each paragraph and list item
  there as a single line, with blank lines between blocks.
- Use a paragraph for explanation and a list for parallel items. Numbered lists
  are for ordered steps, such as adding a chipset.
- Each list item follows the sentence rules, including the rule about starting
  with a word.

**Tables**

- Use a table when readers will look up values across several items, such as
  registers, parameters, error reports or files.
- Always include a header row. The first column is the label that readers scan.
- Cells that hold data or labels, such as `4 bytes`, `40 MSps, fixed` or
  MCU → host, are not sentences. A cell that explains something is a sentence
  and follows the sentence rules.
- Leave the final period off table cells, even when a cell holds two sentences.
- Escape a pipe inside inline code as `\|`.

**Code blocks**

- Always tag the language: `sh` for commands, `c` for C, `text` for formulas
  and byte layouts, `mermaid` for diagrams.
- Shell examples run from the repository root and match what a user should
  type. Do not include a prompt character.
- C examples compile in spirit. When they skip error handling, say so in the
  following sentence.

**Links**

- Use relative links to files in the repository, such as `[usb.c](../src/usb.c)`
  from `docs/`.
- Make link text describe the target, such as "the [filter reference](...)".
  Never write a bare URL or "click here".
- Place a link inside a sentence, never at its start.
- The one exception is an embedded GitHub video or image. GitHub only renders
  the player when the `https://github.com/user-attachments/assets/...` URL sits
  alone on its own line, so leave it bare, as the README demo video does.

**Emphasis and callouts**

- Use bold sparingly for the one fact a reader must not miss, such as
  **no vendor ROM patch loaded** or **not hardware registers**.
- Use italics only to introduce a term.
- Use a GitHub callout (`> [!IMPORTANT]`) only for risk to the user, such as the
  experimental status. Keep it to one per page.

## Use the standard section patterns

**Reference sections in a backend page** follow this order:

1. A short paragraph that says what the stage does and what is known about it.
2. Paragraphs for the sequence or behavior, in the order the code runs.
3. A register table with the columns
   `Mechanism | Access / address | Known fields or current use | Limits`.
4. A closing `Implementation:` line that links the source files, as in
   "Implementation: [mt7921_gain.c](../src/chips/mt7921_gain.c)."

**Behavior during capture**, such as live tuning or gain, covers what changes,
what keeps running, what the setter commits, what happens on failure and what
the caller cannot know, such as the exact transition sample.

**Memory and protocol tables** state clearly when locations are firmware
protocol rather than hardware registers, and give offsets in bytes.

**Test descriptions** say what each test drives, what it injects, what it
checks and what it does not claim. Say whether it runs through CTest or needs
hardware.

**The README** keeps this order: title and one-line summary, the experimental
callout, background with the diagram, specifications table, proof of concept,
build and try, API, automatic recovery, stability tests, install and link,
source layout and firmware, license and acknowledgements. When a value
changes, update the README specifications table and diagram together with the
backend page.

## Vendor and licensing language

- Call chip vendors' software "vendor firmware", "vendor patch" or "vendor ROM
  dump". Do not name the vendor in prose.
- Part numbers (MT7921, MT7961, MT7972, MT7925), USB IDs and board names are
  fine, because readers need them to identify hardware.
- Never edit or remove copyright notices in `LICENSES/` or `NOTICE`. The
  BSD-3-Clause-Clear license requires them to stay as they are.
- Never word anything as an endorsement by a copyright holder or vendor.
- Acknowledgements credit each project inside a sentence that starts with a
  word, as in "The [W1700K SDR](...) project by h0m3us3r, related work
  that...".

## Check before finishing

Run the style checker from the repository root. It exits nonzero when it finds
a problem.

```sh
python3 -I .agents/skills/write-docs/scripts/style_check.py README.md docs/*.md
```

It reports dashes and semicolons outside code, and sentences or list items that
start with an acronym, number, inline code, link or URL. In tables it skips the
first column and the leading value of each cell, since those are labels and
data. Add `--strict` to see leading values too, then judge each one by hand.

Then confirm these points yourself, because no script catches them:

- The content sits in the right file for its reader.
- Every claim uses the right evidence word, and limits sit next to features.
- Links and anchors resolve, including any you added.
- The page reads naturally from top to bottom for its intended reader.
