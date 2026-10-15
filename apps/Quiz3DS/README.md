# Quiz3DS

A multiple-choice quiz developed in C for AuroraOS on the Nintendo 3DS.

## 📖 About

Quiz3DS is an application designed to test your knowledge through multiple-choice questions covering a wide range of topics.

The game includes a database of **2,000 questions** across **10 categories**, with randomly shuffled questions and answers and a scoring system for each game.

The interface is designed for both Nintendo 3DS screens and supports both physical buttons and touchscreen input.

## ✨ Features

### 📚 10 Categories

The quiz covers the following topics:

1. **Science & Space** — science, physics, chemistry, and astronomy.
2. **Technology & Computing** — computers, programming, networking, and cybersecurity.
3. **World History** — historical events, civilizations, and historical figures.
4. **World Geography** — countries, capitals, continents, and geographical locations.
5. **Culture & Society** — traditions, religions, languages, and societies.
6. **Art & Literature** — artworks, authors, and artistic movements.
7. **Movies, TV & Animation** — films, television series, and animation.
8. **Sports** — sporting disciplines, rules, and competitions.
9. **Logic & Mathematics** — logical reasoning, numbers, and sequences.
10. **General Knowledge** — questions covering a variety of subjects.

### 🎮 Gameplay

- 2,000 questions in the database.
- 10 selectable categories.
- 10 randomly selected questions per game.
- Questions are shuffled for each new game.
- Four possible answers per question.
- Answer options are shuffled randomly.
- Scoring system based on correct answers.
- Feedback displayed after each answer.
- Correct answer revealed after answering.
- Final results screen showing the game score.
- Option to start a new game or return to the category menu.

### 🖥️ Interface

- Dedicated graphical interface for the Nintendo 3DS.
- Uses both console screens.
- Touchscreen support.
- Navigation through physical buttons.
- Modern interface with panels, colors, and visual indicators.
- Visual highlighting of the selected category and answer.

## 🎮 Controls

| Button | Function |
|---|---|
| D-pad Up/Down | Navigate categories or answers |
| A | Select a category, confirm an answer, or proceed to the next question |
| B | Return to the category menu |
| Touchscreen | Select categories and answers and interact with the interface |

## 📥 Download

The `.bin` file can be found in the folder dedicated to this application within the repository.

## 🛠️ Development

Quiz3DS is written in **C** and developed for AuroraOS on the Nintendo 3DS.

The project uses:
- C programming language.
- AuroraOS API through `aurora_app.h`.
- Standard C library functions.
- Physical button input.
- Touchscreen input.
- Graphics functions for managing both screens.

The source code is available in this repository.

## 📊 Game Structure

Each game follows this sequence:

1. Select a category.
2. Receive 10 randomly selected questions.
3. Choose one answer from four options.
4. View feedback and the correct answer.
5. Update the score.
6. View the final results.
7. Start another game or return to the category menu.

Each new game uses a newly randomized question order.

## ⚠️ Notes

- Each game contains 10 questions.
- The maximum score per game is 10 correct answers.
- Questions and answers are in English.
- The database contains 2,000 questions and their corresponding answer options.
- Compatibility depends on the AuroraOS environment being used.

## 🙏 Acknowledgements

Special thanks to the **AuroraOS Team** for developing the operating system and making it possible to create applications for the Nintendo 3DS.

- AuroraOS: https://github.com/TeamAuroraOS/AuroraOS

## 🌐 AuroraOS Store

All my applications will be published on the AuroraOS Store, where you will be able to find and access all the apps I develop for AuroraOS on the Nintendo 3DS.

**[Visit the AuroraOS Store](https://account.aurora3ds.xyz/login?next=%2Fstore)**

Log in to your AuroraOS account to access the store and discover my applications.

## 📄 License

This application, including its source code and compiled binaries, is subject to the terms and conditions specified in [`Aurora_App/LICENSE.md`](../../LICENSE.md).

---

**Developed by [DanyCasy](https://github.com/DanyCasy)**

*Part of the Aurora_App project — applications developed for AuroraOS on the Nintendo 3DS.*
