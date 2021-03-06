from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration, EnvironmentVariable, PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare

import yaml
import logging
FORMAT = '[%(levelname)s] [launch]: %(message)s'
logging.basicConfig(format=FORMAT)

def get_controller_node(context, *args, **kwargs):
    config = LaunchConfiguration('config').perform(context)

    with open(config, "r") as f:
        config_params = yaml.safe_load(f)

    try:
        plugin_name = config_params["/**"]["ros__parameters"]["plugin_name"]
    except KeyError:
        plugin_name = ""

    if not plugin_name:
        logging.critical("Plugin not set.")
        exit(-1)

    try:
        plugin_config = config_params["/**"]["ros__parameters"]["plugin_config_file"]
    except KeyError:
        plugin_config = ""

    if not plugin_config:
        plugin_config = PathJoinSubstitution([
            FindPackageShare(plugin_name),
            'config', 'default_controller.yaml'
        ])

    node = Node(
        package='controller_manager',
        executable='controller_manager_node',
        namespace=LaunchConfiguration('drone_id'),
        parameters=[LaunchConfiguration('config'), plugin_config],
        output='screen',
        emulate_tty=True
    )

    return [node]


def generate_launch_description():
    config = PathJoinSubstitution([
        FindPackageShare('controller_manager'),
        'config', 'controller_manager.yaml'
    ])

    ld = LaunchDescription([
        DeclareLaunchArgument('drone_id', default_value=EnvironmentVariable('AEROSTACK2_SIMULATION_DRONE_ID')),
        DeclareLaunchArgument('config', default_value=config),
        OpaqueFunction(function=get_controller_node)
    ])

    return ld
