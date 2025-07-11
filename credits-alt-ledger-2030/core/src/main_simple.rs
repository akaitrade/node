/*!
 * CREDITS ALT-LEDGER 2030 - Simple Standalone Node
 * 
 * Simplified version for testing basic functionality
 */

use std::io::{self, Write};
use serde_json::json;

fn main() {
    println!("🚀 CREDITS ALT-LEDGER 2030 - Simple Node");
    println!("This is a simplified version for testing purposes.");
    
    // Test basic functionality
    println!("✅ Basic imports working");
    
    // Simple interactive loop
    loop {
        print!("simple-node> ");
        io::stdout().flush().unwrap();
        
        let mut input = String::new();
        match io::stdin().read_line(&mut input) {
            Ok(_) => {
                let input = input.trim();
                
                if input == "quit" || input == "exit" {
                    break;
                }
                
                match input {
                    "help" => {
                        println!("Available commands:");
                        println!("  help   - Show this help");
                        println!("  status - Show node status");
                        println!("  test   - Run test");
                        println!("  quit   - Exit");
                    }
                    "status" => {
                        let status = json!({
                            "running": true,
                            "version": "1.0.0",
                            "phase": 3,
                            "simple_mode": true
                        });
                        println!("Status: {}", serde_json::to_string_pretty(&status).unwrap());
                    }
                    "test" => {
                        println!("✅ Basic test passed - imports and JSON working");
                    }
                    _ => {
                        println!("Unknown command: {}. Type 'help' for available commands.", input);
                    }
                }
            }
            Err(e) => {
                println!("Error reading input: {}", e);
            }
        }
    }
    
    println!("👋 Simple node exited");
}