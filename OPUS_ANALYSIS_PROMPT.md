# Comprehensive Repository Analysis Prompt for Claude Opus

Please conduct a thorough analysis of this embedded systems thesis project repository. Focus on the following areas:

## 1. Project Overview & Goals
- Identify the primary research objectives and thesis scope
- Understand what problems the project solves
- Determine the target hardware platform and MCU
- Clarify the relationship between sub-projects (rtos_test, target_proj)

## 2. Architecture & Design Analysis
- Analyze the overall system architecture
- Map core components and their interactions
- Identify key design patterns and principles used
- Evaluate the separation of concerns across modules
- Review how hardware abstraction is implemented

## 3. Key Technical Components
- **RTOS & Real-Time Aspects**: Analyze FreeRTOS integration and task management
- **Communication Protocols**: PMBus, I2C, MQTT, Modbus functionality
- **Data Management**: Persistent buffering strategy (including QSPI), offline buffer handling
- **Networking**: Wi-Fi, AWS IoT SDK integration, LwIP stack
- **Security**: TLS/mTLS implementation, credential handling
- **Reliability Features**: Bus recovery mechanisms, timeout handling, error recovery

## 4. Build System Analysis
- Review ModusToolbox configuration and Makefiles
- Understand build configurations (Debug, Release, QSPI variants)
- Identify toolchain setup (GCC_ARM) and build artifacts
- Analyze dependencies and library management
- Evaluate build optimization opportunities

## 5. Documentation Review
- Summarize architecture documentation
- Review experimental methodology and findings
- Analyze compliance and coursework documentation
- Identify documentation gaps or unclear areas
- Evaluate code documentation quality (Doxygen setup)

## 6. Experiments & Validation
- Understand all experiments (exp1-exp4) and their purpose
- Review testing methodology
- Analyze test results and findings
- Evaluate reliability/robustness testing approach
- Identify potential issues or edge cases not covered

## 7. Code Quality Assessment
- Evaluate coding standards and consistency
- Identify complexity hotspots
- Assess error handling patterns
- Review resource management (memory, peripherals)
- Recommend general improvements

## 8. Hardware Integration
- Map hardware requirements (BOM, wiring)
- Understand peripheral usage (UART, SPI, I2C, etc.)
- Analyze GPIO configuration
- Review clock and power management

## 9. Testing & Validation Coverage
- Identify what is tested and what isn't
- Analyze test infrastructure and test cases
- Evaluate coverage of edge cases
- Review integration testing approach

## 10. Potential Issues & Recommendations
- Identify architectural concerns or design issues
- Flag potential reliability or performance problems
- Recommend code cleanup or refactoring opportunities
- Suggest missing features or robustness improvements
- Identify security or safety concerns
- Recommend documentation improvements

## 11. Project Maturity Assessment
- Evaluate production-readiness
- Assess code stability and polish
- Identify technical debt
- Review configuration management
- Evaluate deployment and provisioning strategy

## Deliverables Expected
Please provide:
1. **Executive Summary** (1-2 pages): High-level overview and key findings
2. **Detailed Analysis** by each section above
3. **Issues & Concerns Matrix**: Severity/impact assessment
4. **Recommendations Prioritized**: Quick wins vs. long-term improvements
5. **Architecture Diagram**: Visual representation of key components
6. **Assessment Scorecard**: Rating critical aspects (0-10 scale)

## Additional Context
- This is a thesis project for diploma completion
- Production-level reliability and robustness are expected
- The project integrates multiple wireless protocols and industrial communication standards
- Compliance and evidence documentation suggests formal requirements
- Multiple experiments indicate research validation is core to project success
