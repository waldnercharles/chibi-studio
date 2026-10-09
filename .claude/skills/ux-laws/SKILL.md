---
name: ux-laws
description: Apply 30 Laws of UX when designing, reviewing, or improving interfaces. Use for usability audits, visual alignment and hierarchy, navigation, forms, controls, interaction feedback, error recovery, and perceived performance across web, mobile, and desktop products.
---

# Laws of UX

Use these principles to connect an observed usability problem to a practical
change and a checkable result. All 30 laws and their application rules are
included below; no separate reference file is needed.

## Apply the laws

1. Identify the user's task, expected outcome, input methods, and relevant
   constraints. Inspect the current interface and comparable controls before
   proposing a change. During an audit, inventory the requested interactions
   and record which ones you actually exercise.
2. Choose the laws that explain the observed problem. State the connection:
   evidence, principle, proposed change, and expected result. A law is a design
   heuristic, not proof that a particular solution will work.
3. Reuse the product's established components, terminology, and design tokens
   where they support the task. Resolve competing principles using user needs,
   accessibility, platform conventions, and measured behavior.
4. Prioritize data loss, blocked tasks, inaccessible controls, and misleading
   state before cosmetic refinements. Preserve the requested scope: an
   assessment ends with findings; an implementation request includes fixes
   and verification.

## Visual hierarchy and perception

### 1. Aesthetic-Usability Effect

People often perceive attractive interfaces as easier to use. This perception
can also conceal usability problems during evaluation.

- Use consistent typography, spacing, alignment, color, and control states.
  Compare text baselines, icon centers, field edges, and visual weight.
- Look for inconsistent styling, clipped content, missing focus or hover
  feedback, and unexplained differences between equivalent controls.
- Verify task success separately from appearance. Add decoration or motion
  only when it supports the task; polish alone does not establish usability.

### 2. Law of Prägnanz (Simplicity)

People tend to interpret complex or ambiguous forms as the simplest coherent
shape available.

- Simplify icons, layouts, and visualizations so their structure is easy to
  recognize. Remove details that compete with meaning.
- Prefer clear silhouettes, readable grouping, and adequate space. Check
  whether users can distinguish essential information at the actual size.
- Preserve necessary labels and distinctions; visual simplicity must not make
  the meaning ambiguous.

### 3. Von Restorff Effect (Isolation)

An item that differs from otherwise similar items is more likely to be noticed
and remembered.

- Give the most consequential action, alert, or datum a deliberate visual
  distinction through contrast, placement, shape, or weight.
- When everything competes for attention, reduce competing emphasis. Keep
  emphasis proportional to importance and avoid relying on color alone.

### 4. Law of Similarity

Elements with similar appearance are perceived as related.

- Give controls with the same role consistent structure and interaction
  states. Align equivalent labels, values, icons, and action rows.
- Distinguish elements with different behavior. Check that links, buttons,
  selectable items, and static labels are not misleadingly interchangeable.

### 5. Law of Proximity

Nearby elements are perceived as belonging together.

- Keep labels, values, help, validation, and related actions close to their
  subject. Use tighter spacing within a group than between unrelated groups.
- Check relationships at narrow and wide sizes. A stretched layout must not
  separate a label from its field or attach an error to the wrong control.

### 6. Law of Common Region

Elements inside a shared boundary are perceived as a group.

- Use a section heading, subtle background, border, or container when it
  clarifies a meaningful relationship.
- Check that boundaries contain the full group and exclude unrelated actions.
  Avoid wrapping every item in a box when spacing already communicates it.

### 7. Law of Uniform Connectedness

Visually connected elements are perceived as related.

- Use connectors for actual relationships in flows, timelines, diagrams, and
  multistep processes. Keep endpoints, direction, and labels unambiguous.
- Check that crossings, broken connections, and decorative lines do not imply
  false relationships. Supply another cue when a connector alone is unclear.

## Cognitive load and decision making

### 8. Cognitive Load

Understanding and operating an interface consumes limited mental resources.

- Remove unnecessary decisions and repeated work. Use clear hierarchy,
  sensible defaults, contextual help, and progressive disclosure.
- Look for competing panels, unfamiliar terms, and tasks that require users
  to remember hidden state. Keep information needed for the current decision
  visible without presenting every advanced option at once.

### 9. Hick's Law

Decision time tends to grow with the number and complexity of choices.

- Organize choices by the user's task. Offer useful search, filters, and
  defaults when a flat list becomes difficult to scan.
- Preserve access to valid options and make them distinguishable. There is no
  universal maximum of five or seven menu items; familiarity and the cost of
  comparing options matter.

### 10. Miller's Law

Working memory has limited capacity, and meaningful chunks affect what people
can retain. The historical seven-plus-or-minus-two observation is not a fixed
limit for navigation, tables, or visible choices.

- Reduce what a user must hold in memory by grouping related information and
  making context available at the point of use.
- Check actual recall demands. Do not remove useful visible options merely to
  satisfy a numerical rule.

### 11. Chunking

Grouping information into meaningful units makes it easier to process.

- Divide long forms, settings, content, and data into recognizable sections
  with descriptive headings and consistent structure.
- Group by meaning or task rather than arbitrary item counts. Check that a
  section can be understood without repeatedly consulting another section.

### 12. Choice Overload

Large or difficult-to-compare choice sets can increase effort and discourage
decisions; the effect depends on context and the user's knowledge.

- Make relevant differences easy to compare. Offer justified recommendations,
  sensible defaults, and access to advanced choices when useful.
- Check whether filtering or grouping resolves the problem before hiding
  options. Explain consequential defaults and let users change them.

### 13. Cognitive Bias

Framing, defaults, and prior expectations can systematically influence judgment.

- Present comparisons in consistent units and disclose relevant costs and
  consequences near the decision. Make defaults visible and reversible when
  the action permits it.
- Check whether wording, ordering, or emphasis steers users against their
  stated interests. Use truthful evidence; never invent activity, scarcity,
  urgency, popularity, or recommendations to pressure a choice.

## Interaction and behavior

### 14. Fitts's Law

The time required to acquire a target depends on its size and distance.

- Give controls adequate effective hit areas and spacing for the input device.
  Keep frequent actions near the task and support appropriate keyboard paths.
- Test actual hit regions, overlapping targets, edge placement, and accidental
  activation. Follow applicable platform and accessibility target requirements
  rather than treating one pixel size as universal.
- Prevent destructive mistakes through clear intent and suitable recovery or
  confirmation; making a destructive target difficult to hit is not a safeguard.

### 15. Doherty Threshold

Prompt system feedback helps users maintain attention and continuity. The
historical roughly 400 ms threshold is a responsiveness heuristic, not a frame
budget or an acceptable delay for every interaction.

- Acknowledge input promptly and show truthful pending, progress, success, and
  failure states. Prevent accidental duplicate actions during pending work.
- Measure input-to-feedback latency and completion time separately. For
  continuous interactions, inspect slow frames as well as averages.
- Use optimistic updates only when the operation supports correct failure
  handling and reconciliation. Animation must not conceal stalls or delay
  usable results; respect reduced-motion preferences.

### 16. Flow

Sustained focus is easier when actions, feedback, and task difficulty support
continuous progress.

- Reduce unnecessary interruptions and context switching. Preserve entered
  work, selection, and relevant context when navigating or recovering.
- Keep frequent actions efficient and offer discoverable shortcuts. Match
  save, commit, cancellation, and undo behavior to the product's contract;
  this principle alone does not justify autosave.

### 17. Goal-Gradient Effect

People may become more motivated as they perceive progress toward a goal.

- Show meaningful steps, completed work, and what remains when a task has a
  defined goal. Make the next action easy to identify.
- Use measured progress when available and an honest indeterminate state when
  it is not. Avoid fabricated percentages or artificial near-completion states.

## Memory and recall

### 18. Serial Position Effect

People often remember the first and last items in a sequence more readily.

- Place essential orientation and consequential information where users are
  likely to encounter them. Make a process's final action easy to find.
- Keep familiar ordering stable. Do not move controls unexpectedly or bury
  required information merely to emphasize the ends of a list.

### 19. Peak-End Rule

Intense moments and the ending can disproportionately influence how people
remember an experience.

- Give failures clear explanations and usable recovery. End successful tasks
  with an accurate outcome and a useful next action when one is needed.
- Inspect stressful transitions, empty states, and completion screens. Keep
  feedback proportional; routine tasks rarely need an interrupting celebration.

### 20. Zeigarnik Effect

Unfinished or interrupted tasks can remain more salient than completed ones.

- Make drafts, pending work, and unfinished steps understandable. Provide a
  clear way to resume, finish, or deliberately discard them.
- Preserve enough context to resume without reconstructing the task. Avoid
  nagging, artificial obligations, or pressure to maintain meaningless streaks.

### 21. Working Memory

People can retain and manipulate only a limited amount of information at once.

- Keep instructions, units, prior selections, and relevant comparisons visible
  where they are needed. Favor recognition over recall.
- Use summaries, breadcrumbs, and inline context when they reduce memory
  demands. Preserve form state and avoid requiring users to copy information
  mentally between screens.

## System design principles

### 22. Jakob's Law

People bring expectations learned from other products and platforms.

- Use familiar control behavior, terminology, navigation, and platform
  conventions. Compare a changed interaction with equivalent existing controls.
- Depart from convention when it solves a demonstrated problem, and make the
  new behavior discoverable. Familiarity does not require copying another
  product's appearance or assuming one layout works on every platform.

### 23. Postel's Law (Robustness)

Accept reasonable variations in input while producing clear, predictable output.

- Normalize harmless input differences, such as surrounding whitespace, when
  their meaning is unambiguous. Explain invalid input near its source and
  preserve the user's entry so it can be corrected.
- Preserve types, units, precision, and user intent. Numeric presentation and
  serialization should remain readable without silently changing the value.
- Reject ambiguous or unsafe input rather than guessing. Flexible input does
  not justify inconsistent validation or silently accepting corrupt state.

### 24. Tesler's Law (Conservation of Complexity)

Some complexity belongs to the problem itself; design determines who must
handle it and when.

- Let the system handle mechanical work through appropriate defaults,
  computation, and automation. Expose meaningful decisions at the right time.
- Check that simplification has not merely hidden costs, errors, or necessary
  control. Make consequential automation understandable and correctable.

### 25. Occam's Razor

Among solutions that meet the need, prefer the one requiring fewer unnecessary
assumptions and complications.

- Remove redundant controls, steps, concepts, and decorative structures.
  Choose the simplest solution that preserves required behavior.
- Verify that removing a visible affordance does not replace it with hidden
  gestures, extra navigation, or greater memory demands.

### 26. Pareto Principle (80/20)

A relatively small set of causes or workflows often accounts for a large share
of outcomes. The ratio is a prioritization heuristic, not a measured fact.

- Use evidence about frequency, impact, and failure cost to prioritize work.
  Improve the paths that most strongly affect task success.
- Retain access to necessary less-frequent actions. Do not use usage frequency
  alone to dismiss accessibility, recovery, or rare but consequential tasks.

## Attention and focus

### 27. Selective Attention

People attend to relevant stimuli and may miss information competing with
their current focus.

- Establish a clear hierarchy and place feedback near the action it explains.
  Make focus, selection, errors, and pending state distinguishable.
- Reduce competing emphasis while retaining readable contrast. Use text,
  shape, or another cue alongside color, and check keyboard and assistive
  technology access where applicable.

### 28. Mental Model

People use an internal understanding of a system to predict what actions will do.

- Use terms and relationships that match the user's task. Make ownership,
  current state, and action consequences understandable.
- Test whether visible state matches actual results after edits, navigation,
  cancellation, saving, and recovery. Explain consequential behavior that
  differs from reasonable expectations.

### 29. Paradox of the Active User

People often start using a product before reading its instructions.

- Make common tasks discoverable through clear labels, visible affordances,
  useful defaults, and contextual help at the point of need.
- Let users learn through recoverable actions. Avoid making documentation or
  a long tutorial a prerequisite for basic tasks; keep deeper help accessible.

### 30. Parkinson's Law

Work can expand to fill the time available. In interface design, use this as a
prompt to reduce unnecessary effort and clarify completion.

- Bound tasks with clear outcomes, remove busywork, and provide useful reuse,
  defaults, and shortcuts.
- Check whether the interface creates extra decisions or repetitive steps.
  Do not derive arbitrary deadlines, input limits, or time pressure from this
  principle; limits should serve an actual task or system constraint.

## Verify the result

- Exercise relevant pointer, touch, keyboard, and assistive technology paths.
  Check focus order, activation, editing, commit, cancellation, invalid input,
  empty and disabled states, pending work, failure, and recovery as applicable.
  Verify the underlying result as well as the visible feedback.
- Compare before and after rendering at the affected sizes, text scales,
  content lengths, themes, and states. Inspect baselines, icon alignment,
  spacing within and between groups, popup anchors, clipping, and wrapping.
  Check transitions separately from settled layout.
- For performance work, record a comparable baseline, workload, environment,
  build settings, and measurement method. Distinguish input latency, operation
  time, rendering work, and scheduling. Retain slow samples and keep claims
  proportional to what was measured.
- Follow the project's validation requirements. Add regression coverage when
  it protects meaningful behavior or rendering invariants; avoid tests that
  merely repeat implementation details.
- Report observed problems, relevant laws, changes or recommendations, and
  evidence. Distinguish interactions tested from those only inspected or left
  unverified. Keep the report proportional to the request.

## Attribution

Adapted from the [30-law guide by keysjoao/laws-of-ux-skills](https://raw.githubusercontent.com/keysjoao/laws-of-ux-skills/refs/heads/main/laws-of-ux/references/ux-laws-complete.md),
accessed on 2026-10-02, which attributes its definitions to
[Laws of UX](https://lawsofux.com/). The guidance here applies those concepts
across products and platforms; the links provide attribution, not required
reading.
