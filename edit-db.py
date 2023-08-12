# Run instructions:
# Install packages with "python3 -m pip install prompt_toolkit"
# Run program with "python3 edit-db.py"
import sqlite3
from prompt_toolkit import PromptSession
from prompt_toolkit.history import FileHistory

def run_query(db_path, query):
    try:
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()

        cursor.execute(query)
        rows = cursor.fetchall()
        for row in rows:
            print(row)
    except sqlite3.Error as error:
        print("Could not execute query:", error)
    finally:
        if conn:
            conn.close()

if __name__ == "__main__":
    db_path = input("Enter database path (enter defaults to 'rplace_bot.db'): ")
    if not db_path:
        db_path = "rplace_bot.db"

    history = FileHistory("edit_db_history.txt")
    session = PromptSession(history=history)

    while True:
        try:
            query = session.prompt("> ")
            run_query(db_path, query)
        except KeyboardInterrupt:
            print("KeyboardInterrupt: Press Ctrl-D or type 'exit' to quit")
        except EOFError:
            print("Exiting...")
            break